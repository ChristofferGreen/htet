// Derive the first legal input for a joint N6 transition reconstruction.
//
// The normal-offset collar's underside was useful as a collar diagnostic, but
// it is not an immutable interface.  This probe explicitly removes it from
// the prescribed boundary.  What remains is the exact visible DC sheet plus
// the finite fixture curtain: one open, oriented exterior front whose sole
// rim is where a jointly-owned collar/buffer reconstruction may attach.  The
// exact 96-tet terraced core is retained as the other immutable input.
//
// No fill is emitted here.  In particular, neither a regular-column proxy nor
// a closed fan is smuggled back in as a lower front.  This is an executable
// contract for the next cavity constructor, and a rejection of treating the
// old collar underside as fixed.
#define main export_two_front_complete_private_main
#include "export_two_front_complete.cpp"
#undef main

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
using Face = std::array<std::uint64_t, 3>;
using Tet = std::array<std::uint64_t, 4>;

struct OpenFrontAttempt {
  std::size_t visible_faces{};
  std::size_t fixture_curtain_faces{};
  std::size_t removed_artificial_inner_faces{};
  std::size_t open_front_faces{};
  std::size_t rim_edges{};
  std::size_t nonmanifold_front_edges{};
  std::size_t front_components{};
  std::size_t core_tets{};
  std::size_t core_boundary_faces{};
  std::size_t literal_front_core_faces{};
  std::size_t collar_core_strict_overlaps{};
  bool visible_exact{};
  bool fixture_exterior_exact{};
  bool artificial_inner_excluded{};
  bool open_single_component{};
  bool core_exact{};
  bool deterministic{};
  std::size_t work_items{};
  std::size_t retained_bytes{};
  std::size_t temporary_bytes{};
};

std::set<Face> one_use_faces(const std::vector<Tet>& tets) {
  std::map<Face, unsigned> uses;
  for (const auto& tet : tets) for (const auto local : tet_faces)
    ++uses[face_key({{tet[local[0]], tet[local[1]], tet[local[2]]}})];
  std::set<Face> result;
  for (const auto& [face, count] : uses) if (count == 1U) result.insert(face);
  return result;
}

OpenFrontAttempt build_open_front_attempt(bool reverse) {
  const auto config = fixture_config("n6");
  auto surface = dual_contour_surface(config, 0U, 2U * config.resolution);
  if (reverse) std::reverse(surface.triangles.begin(), surface.triangles.end());
  const auto candidate = make_candidate(
      config, surface, kCanonicalOffsetInCells / static_cast<double>(config.resolution));

  std::vector<Tet> collar_tets;
  collar_tets.reserve(candidate.collar.tets.size());
  for (const auto& tet : candidate.collar.tets) collar_tets.push_back(tet.vertices);
  const auto collar_boundary = one_use_faces(collar_tets);

  OpenFrontAttempt result;
  result.visible_faces = candidate.collar.frozen_outer.size();
  result.fixture_curtain_faces = candidate.collar.expected_curtain.size();
  result.removed_artificial_inner_faces = candidate.collar.expected_inner.size();
  const auto expected_open = [&] {
    auto faces = candidate.collar.frozen_outer;
    faces.insert(candidate.collar.expected_curtain.begin(), candidate.collar.expected_curtain.end());
    return faces;
  }();
  result.open_front_faces = expected_open.size();
  result.visible_exact = candidate.accepted &&
      std::includes(collar_boundary.begin(), collar_boundary.end(),
                    candidate.collar.frozen_outer.begin(), candidate.collar.frozen_outer.end());
  result.fixture_exterior_exact = std::includes(
      collar_boundary.begin(), collar_boundary.end(),
      candidate.collar.expected_curtain.begin(), candidate.collar.expected_curtain.end());
  result.artificial_inner_excluded = true;
  for (const auto& face : candidate.collar.expected_inner)
    result.artificial_inner_excluded = result.artificial_inner_excluded &&
        !expected_open.contains(face) && collar_boundary.contains(face);

  // An open front has exactly the inner-front rim: all other edges are used
  // twice.  This makes the formerly hidden movable interface explicit without
  // accidentally calling it a closed volume boundary.
  std::map<std::array<std::uint64_t, 2>, unsigned> edge_uses;
  std::map<std::uint64_t, std::set<std::uint64_t>> graph;
  for (const auto& face : expected_open) for (unsigned i = 0; i < 3U; ++i) {
    auto a = face[i], b = face[(i + 1U) % 3U];
    graph[a].insert(b); graph[b].insert(a);
    if (b < a) std::swap(a, b);
    ++edge_uses[{{a, b}}];
  }
  for (const auto& [edge, count] : edge_uses) {
    (void)edge;
    if (count == 1U) ++result.rim_edges;
    else if (count != 2U) ++result.nonmanifold_front_edges;
  }
  std::set<std::uint64_t> seen;
  for (const auto& [seed, ignored] : graph) {
    (void)ignored;
    if (!seen.insert(seed).second) continue;
    ++result.front_components;
    std::vector<std::uint64_t> pending{seed};
    while (!pending.empty()) {
      const auto current = pending.back(); pending.pop_back();
      for (const auto next : graph.at(current)) if (seen.insert(next).second) pending.push_back(next);
    }
  }
  result.open_single_component = result.rim_edges > 0U &&
      result.nonmanifold_front_edges == 0U && result.front_components == 1U;

  std::map<std::uint64_t, Vec3> vertices = candidate.collar.vertices;
  std::vector<Tet> core;
  for (const auto& source : conservative_core(config)) {
    Tet tet{};
    for (unsigned i = 0; i < 4U; ++i) {
      tet[i] = core_id(source[i]);
      vertices.emplace(tet[i], lattice_position({KeyKind::lattice, source[i]}, config.resolution));
    }
    core.push_back(tet);
  }
  const auto core_boundary = one_use_faces(core);
  result.core_tets = core.size();
  result.core_boundary_faces = core_boundary.size();
  result.core_exact = result.core_tets == conservative_core(config).size() &&
      result.core_tets == 96U && result.core_boundary_faces == 104U;
  for (const auto& face : expected_open)
    result.literal_front_core_faces += core_boundary.contains(face) ? 1U : 0U;

  DualVolumeBuild view; view.vertices = vertices;
  for (const auto& collar : collar_tets) for (const auto& core_tet : core)
    if (dual_tets_strictly_overlap(view, {collar, DualVolumeRegion::transition},
                                   {core_tet, DualVolumeRegion::core}))
      ++result.collar_core_strict_overlaps;
  result.work_items = collar_tets.size() * core.size() + collar_boundary.size() +
      core_boundary.size() + edge_uses.size();
  result.retained_bytes = vertices.size() * sizeof(*vertices.begin()) +
      (collar_tets.size() + core.size()) * sizeof(Tet);
  result.temporary_bytes = edge_uses.size() * sizeof(*edge_uses.begin()) +
      graph.size() * sizeof(*graph.begin());
  return result;
}

int open_front_main() {
  const auto start = std::chrono::steady_clock::now();
  auto forward = build_open_front_attempt(false);
  const auto reverse = build_open_front_attempt(true);
  forward.deterministic = forward.visible_faces == reverse.visible_faces &&
      forward.fixture_curtain_faces == reverse.fixture_curtain_faces &&
      forward.removed_artificial_inner_faces == reverse.removed_artificial_inner_faces &&
      forward.open_front_faces == reverse.open_front_faces && forward.rim_edges == reverse.rim_edges &&
      forward.nonmanifold_front_edges == reverse.nonmanifold_front_edges &&
      forward.front_components == reverse.front_components && forward.core_boundary_faces == reverse.core_boundary_faces &&
      forward.literal_front_core_faces == reverse.literal_front_core_faces &&
      forward.collar_core_strict_overlaps == reverse.collar_core_strict_overlaps;
  const bool prepared = forward.visible_exact && forward.fixture_exterior_exact &&
      forward.artificial_inner_excluded && forward.open_single_component && forward.core_exact &&
      forward.literal_front_core_faces == 0U && forward.collar_core_strict_overlaps == 0U &&
      forward.deterministic;
  std::cout << std::setprecision(17)
    << "{\"probe\":\"dc_n6_open_rebuildable_inner_front/v1\",\"fixture\":\"n6\","
    << "\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_exterior\",\"exact_retained_core\"],\"rebuildable\":[\"collar_underside\",\"buffer\"]},"
    << "\"open_front\":{\"visible_faces\":" << forward.visible_faces
    << ",\"fixture_curtain_faces\":" << forward.fixture_curtain_faces
    << ",\"removed_artificial_inner_faces\":" << forward.removed_artificial_inner_faces
    << ",\"faces\":" << forward.open_front_faces << ",\"rim_edges\":" << forward.rim_edges
    << ",\"nonmanifold_edges\":" << forward.nonmanifold_front_edges
    << ",\"components\":" << forward.front_components << "},"
    << "\"retained_core\":{\"tets\":" << forward.core_tets
    << ",\"boundary_faces\":" << forward.core_boundary_faces
    << ",\"literal_shared_faces\":" << forward.literal_front_core_faces
    << ",\"collar_core_strict_overlaps\":" << forward.collar_core_strict_overlaps << "},"
    << "\"invariants\":{\"visible_dc_exact\":" << (forward.visible_exact ? "true" : "false")
    << ",\"fixture_exterior_exact\":" << (forward.fixture_exterior_exact ? "true" : "false")
    << ",\"artificial_inner_excluded\":" << (forward.artificial_inner_excluded ? "true" : "false")
    << ",\"open_front_is_single_manifold_component\":" << (forward.open_single_component ? "true" : "false")
    << ",\"retained_core_exact\":" << (forward.core_exact ? "true" : "false")
    << ",\"reversed_input_deterministic\":" << (forward.deterministic ? "true" : "false") << "},"
    << "\"resources\":{\"work_items\":" << forward.work_items << ",\"retained_bytes\":" << forward.retained_bytes
    << ",\"temporary_bytes\":" << forward.temporary_bytes << "},"
    << "\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"old_collar_underside_is_not_a_fixed_core_front;_new_open_front_requires_joint_cavity_construction\",\"validated\":" << (prepared ? "true" : "false") << "},\"elapsed_ms\":"
    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << "}\n";
  return prepared ? 0 : 1;
}
} // namespace

#ifdef N6_OPEN_REBUILDABLE_INNER_FRONT_TEST
int n6_open_rebuildable_inner_front_main() { return open_front_main(); }
#else
int main() { try { return open_front_main(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; } }
#endif
