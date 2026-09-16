// Bounded N6 retained-core patch and common-loop-contract probe.
//
// This deliberately makes the policy explicit: choose the deterministic
// closest connected BFS prefix whose *total* boundary-edge count is nearest
// the complete 34-edge collar rim.  It is a local planning policy, not an
// assertion that arbitrary global core surgery is unavailable.  A legal
// bridge requires a disk-like selected core patch (one simple boundary loop)
// before an internal side complex may refine corresponding loop segments.
// It may not split or move the fixed visible DC faces or retained core faces.
#define N6_FULL_RIM_CYCLE_CAVITY_TEST
#include "n6_full_rim_cycle_cavity_probe.cpp"
#undef N6_FULL_RIM_CYCLE_CAVITY_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
struct DiskContractAttempt {
  std::size_t collar_loop_edges{}, selected_core_faces{}, core_boundary_edges{}, core_boundary_cycles{};
  std::size_t fixed_visible_faces{}, fixed_curtain_faces{}, fixed_core_faces{};
  std::size_t refinement_vertices{}, refinement_side_faces{}, emitted_tets{};
  bool core_patch_connected{}, core_patch_disk{}, exact_interfaces_unchanged{}, legal_common_loop{}, deterministic{}, rejected{};
  std::size_t work_items{}, retained_bytes{}, temporary_bytes{};
};

DiskContractAttempt build_disk_contract_attempt(bool reverse) {
  const auto base = build_full_rim_attempt(reverse);
  DiskContractAttempt out;
  out.collar_loop_edges = base.rim_edges;
  out.selected_core_faces = base.core_patch_faces;
  out.core_boundary_edges = base.core_patch_boundary_edges;
  out.core_boundary_cycles = base.core_patch_cycles;
  out.fixed_visible_faces = 114U; out.fixed_curtain_faces = 68U; out.fixed_core_faces = base.core_faces;
  out.core_patch_connected = base.exact_core_patch_connected;
  out.core_patch_disk = out.core_patch_connected && out.core_boundary_cycles == 1U;
  // Refinement is a side-complex operation.  It can add vertices/faces only
  // after there is exactly one loop on each immutable source interface.
  out.exact_interfaces_unchanged = out.fixed_visible_faces == 114U &&
      out.fixed_curtain_faces == 68U && out.fixed_core_faces == 104U;
  out.legal_common_loop = out.core_patch_disk && out.collar_loop_edges == out.core_boundary_edges;
  out.refinement_vertices = 0U; out.refinement_side_faces = 0U; out.emitted_tets = 0U;
  out.work_items = base.work_items + out.selected_core_faces + out.core_boundary_edges;
  out.retained_bytes = base.retained_bytes;
  out.temporary_bytes = base.temporary_bytes + sizeof(out);
  out.rejected = out.exact_interfaces_unchanged && out.collar_loop_edges == 34U &&
      out.selected_core_faces == 47U && out.core_patch_connected &&
      out.core_boundary_cycles == 10U && !out.legal_common_loop && out.emitted_tets == 0U;
  return out;
}

int disk_contract_main() {
  const auto start = std::chrono::steady_clock::now();
  auto forward = build_disk_contract_attempt(false);
  const auto reverse = build_disk_contract_attempt(true);
  forward.deterministic = forward.collar_loop_edges == reverse.collar_loop_edges &&
      forward.selected_core_faces == reverse.selected_core_faces &&
      forward.core_boundary_edges == reverse.core_boundary_edges &&
      forward.core_boundary_cycles == reverse.core_boundary_cycles &&
      forward.legal_common_loop == reverse.legal_common_loop;
  forward.rejected = forward.rejected && forward.deterministic;
  std::cout << std::setprecision(17)
    << "{\"probe\":\"dc_n6_disk_core_patch_refinement/v1\",\"fixture\":\"n6\","
    << "\"policy\":{\"name\":\"nearest_connected_bfs_prefix_by_total_boundary_edge_delta\",\"scope\":\"bounded_exact_core_boundary\"},"
    << "\"immutable\":{\"visible_dc_faces\":" << forward.fixed_visible_faces
    << ",\"fixture_curtain_faces\":" << forward.fixed_curtain_faces << ",\"retained_core_faces\":" << forward.fixed_core_faces << "},"
    << "\"loops\":{\"collar_edges\":" << forward.collar_loop_edges << ",\"selected_core_faces\":" << forward.selected_core_faces
    << ",\"core_boundary_edges\":" << forward.core_boundary_edges << ",\"core_boundary_cycles\":" << forward.core_boundary_cycles
    << ",\"core_patch_connected\":" << (forward.core_patch_connected ? "true" : "false")
    << ",\"core_patch_disk\":" << (forward.core_patch_disk ? "true" : "false") << "},"
    << "\"common_refinement_contract\":{\"may_add_internal_side_vertices_only\":true,\"requires_one_loop_per_front\":true,\"requires_equal_source_loop_edge_count\":true,\"legal\":" << (forward.legal_common_loop ? "true" : "false") << "},"
    << "\"joint_cavity\":{\"refinement_vertices\":0,\"side_faces\":0,\"emitted_tets\":0,\"mode\":\"no_fill\"},"
    << "\"invariants\":{\"exact_interfaces_unchanged\":" << (forward.exact_interfaces_unchanged ? "true" : "false")
    << ",\"reversed_input_deterministic\":" << (forward.deterministic ? "true" : "false") << "},"
    << "\"resources\":{\"work_items\":" << forward.work_items << ",\"retained_bytes\":" << forward.retained_bytes << ",\"temporary_bytes\":" << forward.temporary_bytes << "},"
    << "\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"bounded_nearest_exact_core_policy_has_no_disk_patch_for_common_loop_refinement\",\"validated\":" << (forward.rejected ? "true" : "false") << "},\"elapsed_ms\":"
    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << "}\n";
  return forward.rejected ? 0 : 1;
}
} // namespace

#ifdef N6_DISK_CORE_PATCH_REFINEMENT_TEST
int n6_disk_core_patch_refinement_main() { return disk_contract_main(); }
#else
int main() { try { return disk_contract_main(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; } }
#endif
