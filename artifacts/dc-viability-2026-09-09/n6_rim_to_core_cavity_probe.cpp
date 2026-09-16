// Smallest genuinely rim-to-core N6 construction.
//
// This consumes a real edge of the rebuildable-front rim and one exact face
// of the immutable terraced core.  A deterministic triangular prism provides
// the smallest volume between them.  The rest of the visible DC sheet and the
// finite curtain are retained as the prescribed exterior.  It is intentionally
// an executable rejection if that local attachment leaves the rest of the rim
// open: that is the precise reason a complete solution must jointly retriangulate a
// collar neighbourhood, rather than append a separate side fan.
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

struct RimCoreAttempt {
  std::size_t visible_faces{}, curtain_faces{}, rim_edges{};
  std::size_t core_tets{}, core_faces{}, bridge_tets{};
  std::size_t bridge_core_faces{}, bridge_rim_edges{};
  std::size_t nonpositive{}, boundary_invalid_edges{}, boundary_components{};
  bool visible_exact{}, fixture_exact{}, core_exact{}, deterministic{}, rejected{};
  double nearest_pair_distance{};
  std::size_t work_items{}, retained_bytes{}, temporary_bytes{};
};

std::set<Face> cavity_one_use_faces(const std::vector<Tet>& tets) {
  std::map<Face, unsigned> uses;
  for (const auto& tet : tets) for (const auto local : tet_faces)
    ++uses[face_key({{tet[local[0]], tet[local[1]], tet[local[2]]}})];
  std::set<Face> result;
  for (const auto& [face, count] : uses) if (count == 1U) result.insert(face);
  return result;
}

Vec3 face_centre(const std::map<std::uint64_t, Vec3>& v, const Face& f) {
  return (v.at(f[0]) + v.at(f[1]) + v.at(f[2])) / 3.0;
}

RimCoreAttempt build_rim_core_attempt(bool reverse) {
  const auto config = fixture_config("n6");
  auto surface = dual_contour_surface(config, 0U, 2U * config.resolution);
  if (reverse) std::reverse(surface.triangles.begin(), surface.triangles.end());
  const auto candidate = make_candidate(config, surface,
      kCanonicalOffsetInCells / static_cast<double>(config.resolution));
  std::vector<Tet> collar;
  for (const auto& tet : candidate.collar.tets) collar.push_back(tet.vertices);
  const auto collar_boundary = cavity_one_use_faces(collar);
  auto exterior = candidate.collar.frozen_outer;
  exterior.insert(candidate.collar.expected_curtain.begin(), candidate.collar.expected_curtain.end());

  std::map<std::array<std::uint64_t, 2>, unsigned> rim_uses;
  for (const auto& face : exterior) for (unsigned i = 0; i != 3U; ++i) {
    auto a = face[i], b = face[(i + 1U) % 3U]; if (b < a) std::swap(a, b);
    ++rim_uses[{{a, b}}];
  }
  std::vector<std::array<std::uint64_t, 2>> rim;
  for (const auto& [edge, uses] : rim_uses) if (uses == 1U) rim.push_back(edge);

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
  const auto core_boundary = cavity_one_use_faces(core);

  // Canonical nearest edge/face selection gives the smallest concrete bridge
  // while keeping reversal of input traversal irrelevant.
  std::array<std::uint64_t, 2> chosen_rim{}; Face chosen_core{};
  double best = std::numeric_limits<double>::infinity();
  for (const auto& edge : rim) for (const auto& face : core_boundary) {
    const auto midpoint = (vertices.at(edge[0]) + vertices.at(edge[1])) / 2.0;
    const auto d = dot(midpoint - face_centre(vertices, face), midpoint - face_centre(vertices, face));
    if (d < best || (d == best && std::pair{edge, face} < std::pair{chosen_rim, chosen_core})) {
      best = d; chosen_rim = edge; chosen_core = face;
    }
  }
  constexpr std::uint64_t kShoulder = 0x7300000000000000ULL;
  const auto midpoint = (vertices.at(chosen_rim[0]) + vertices.at(chosen_rim[1])) / 2.0;
  vertices.emplace(kShoulder, (midpoint + face_centre(vertices, chosen_core)) / 2.0);
  const Face shoulder{{chosen_rim[0], chosen_rim[1], kShoulder}};
  std::array<unsigned, 3> permutation{{0, 1, 2}};
  double pairing = std::numeric_limits<double>::infinity();
  std::array<unsigned, 3> p{{0, 1, 2}};
  do {
    double sum{}; for (unsigned i = 0; i != 3U; ++i) {
      const auto d = vertices.at(shoulder[i]) - vertices.at(chosen_core[p[i]]); sum += dot(d, d);
    }
    if (sum < pairing) { pairing = sum; permutation = p; }
  } while (std::next_permutation(p.begin(), p.end()));
  const Face lower{{chosen_core[permutation[0]], chosen_core[permutation[1]], chosen_core[permutation[2]]}};
  std::vector<Tet> bridge{{{{shoulder[0], shoulder[1], shoulder[2], lower[0]}},
                           {{shoulder[1], shoulder[2], lower[0], lower[1]}},
                           {{shoulder[2], lower[0], lower[1], lower[2]}}}};
  RimCoreAttempt out;
  for (auto& tet : bridge) {
    const auto six = signed_six_volume(vertices.at(tet[0]), vertices.at(tet[1]), vertices.at(tet[2]), vertices.at(tet[3]));
    if (six < 0.0) std::swap(tet[1], tet[2]);
    if (std::abs(six) <= 1.0e-13) ++out.nonpositive;
  }
  std::vector<Tet> all = core; all.insert(all.end(), bridge.begin(), bridge.end());
  const auto actual_boundary = cavity_one_use_faces(all);
  std::map<std::array<std::uint64_t, 2>, unsigned> boundary_edges;
  std::map<std::uint64_t, std::set<std::uint64_t>> graph;
  for (const auto& face : actual_boundary) for (unsigned i = 0; i != 3U; ++i) {
    auto a = face[i], b = face[(i + 1U) % 3U]; graph[a].insert(b); graph[b].insert(a);
    if (b < a) std::swap(a, b); ++boundary_edges[{{a, b}}];
  }
  // Include the immutable open exterior in the boundary audit: it is exactly
  // this shared rim edge that reveals the attempted append is not a manifold
  // assembly of the prescribed exterior and retained core.
  for (const auto& face : exterior) for (unsigned i = 0; i != 3U; ++i) {
    auto a = face[i], b = face[(i + 1U) % 3U]; graph[a].insert(b); graph[b].insert(a);
    if (b < a) std::swap(a, b); ++boundary_edges[{{a, b}}];
  }
  for (const auto& [edge, uses] : boundary_edges) { (void)edge; if (uses != 2U) ++out.boundary_invalid_edges; }
  std::set<std::uint64_t> seen;
  for (const auto& [seed, ignored] : graph) { (void)ignored; if (!seen.insert(seed).second) continue;
    ++out.boundary_components; std::vector<std::uint64_t> pending{seed};
    while (!pending.empty()) { const auto here = pending.back(); pending.pop_back();
      for (const auto next : graph.at(here)) if (seen.insert(next).second) pending.push_back(next); }
  }
  out.visible_faces = candidate.collar.frozen_outer.size(); out.curtain_faces = candidate.collar.expected_curtain.size();
  out.rim_edges = rim.size(); out.core_tets = core.size(); out.core_faces = core_boundary.size(); out.bridge_tets = bridge.size();
  out.bridge_core_faces = actual_boundary.contains(face_key(lower)) ? 0U : 1U;
  out.bridge_rim_edges = rim_uses.at(chosen_rim) == 1U ? 1U : 0U;
  out.visible_exact = std::includes(collar_boundary.begin(), collar_boundary.end(), candidate.collar.frozen_outer.begin(), candidate.collar.frozen_outer.end());
  out.fixture_exact = std::includes(collar_boundary.begin(), collar_boundary.end(), candidate.collar.expected_curtain.begin(), candidate.collar.expected_curtain.end());
  out.core_exact = core.size() == 96U && core_boundary.size() == 104U;
  out.nearest_pair_distance = std::sqrt(best);
  out.work_items = rim.size() * core_boundary.size() + all.size() + boundary_edges.size();
  out.retained_bytes = vertices.size() * sizeof(*vertices.begin()) + all.size() * sizeof(Tet);
  out.temporary_bytes = rim_uses.size() * sizeof(*rim_uses.begin()) + boundary_edges.size() * sizeof(*boundary_edges.begin());
  out.rejected = out.visible_exact && out.fixture_exact && out.core_exact && out.rim_edges == 34U &&
      out.bridge_tets == 3U && out.bridge_core_faces == 1U && out.bridge_rim_edges == 1U &&
      out.nonpositive == 0U && out.boundary_invalid_edges > 0U;
  return out;
}

int rim_core_main() {
  const auto start = std::chrono::steady_clock::now();
  auto forward = build_rim_core_attempt(false), reversed = build_rim_core_attempt(true);
  forward.deterministic = forward.rim_edges == reversed.rim_edges && forward.core_faces == reversed.core_faces &&
      forward.bridge_tets == reversed.bridge_tets && forward.bridge_core_faces == reversed.bridge_core_faces &&
      forward.bridge_rim_edges == reversed.bridge_rim_edges && forward.nonpositive == reversed.nonpositive &&
      forward.boundary_invalid_edges == reversed.boundary_invalid_edges;
  forward.rejected = forward.rejected && forward.deterministic;
  std::cout << std::setprecision(17) << "{\"probe\":\"dc_n6_rim_to_core_cavity/v1\",\"fixture\":\"n6\","
    << "\"assembly\":{\"visible_dc_faces\":" << forward.visible_faces << ",\"fixture_curtain_faces\":" << forward.curtain_faces
    << ",\"open_rim_edges\":" << forward.rim_edges << ",\"retained_core_tets\":" << forward.core_tets
    << ",\"retained_core_faces\":" << forward.core_faces << ",\"bridge_tets\":" << forward.bridge_tets
    << ",\"consumed_core_faces\":" << forward.bridge_core_faces << ",\"consumed_rim_edges\":" << forward.bridge_rim_edges << "},"
    << "\"audit\":{\"nonpositive_tets\":" << forward.nonpositive << ",\"boundary_invalid_edges\":" << forward.boundary_invalid_edges
    << ",\"boundary_components\":" << forward.boundary_components << ",\"nearest_rim_core_distance\":" << forward.nearest_pair_distance << "},"
    << "\"invariants\":{\"visible_dc_exact\":" << (forward.visible_exact ? "true" : "false") << ",\"fixture_exterior_exact\":" << (forward.fixture_exact ? "true" : "false")
    << ",\"retained_core_exact\":" << (forward.core_exact ? "true" : "false") << ",\"reversed_input_deterministic\":" << (forward.deterministic ? "true" : "false") << "},"
    << "\"resources\":{\"work_items\":" << forward.work_items << ",\"retained_bytes\":" << forward.retained_bytes << ",\"temporary_bytes\":" << forward.temporary_bytes << "},"
    << "\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"one_rim_edge_to_one_core_face_prism_leaves_the_prescribed_rim_open\",\"validated\":" << (forward.rejected ? "true" : "false") << "},\"elapsed_ms\":" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << "}\n";
  return forward.rejected ? 0 : 1;
}
} // namespace

#ifdef N6_RIM_TO_CORE_CAVITY_TEST
int n6_rim_to_core_cavity_main() { return rim_core_main(); }
#else
int main() { try { return rim_core_main(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; } }
#endif
