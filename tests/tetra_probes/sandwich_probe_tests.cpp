#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <algorithm>
#include <bit>
#include <map>
#include <utility>
#include <tuple>

#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/bcc_transition_request.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"

namespace {
tetra::probes::SharedLatticeZipperRequest mismatched_square_zipper_request() {
  tetra::probes::SharedLatticeZipperRequest request;
  request.surface.stable_vertex_ids={10U,11U,12U,13U};
  request.surface.vertices={{{0.0,0.0,1.0}},{{1.0,0.0,1.08}},
                            {{1.0,1.0,0.94}},{{0.0,1.0,1.03}}};
  // The frozen surface uses the opposite diagonal from the Freudenthal top.
  request.surface.triangles={{{0U,1U,3U}},{{1U,2U,3U}}};
  request.surface.boundary_edges={{{0U,1U}},{{1U,2U}},{{2U,3U}},{{3U,0U}}};
  request.surface.validation.valid=true;
  request.core.stable_vertex_ids={20U,21U,22U,23U,24U,25U,26U,27U};
  request.core.vertices={{{0.0,0.0,0.0}},{{1.0,0.0,0.0}},{{1.0,1.0,0.0}},{{0.0,1.0,0.0}},
                         {{0.0,0.0,-1.0}},{{1.0,0.0,-1.0}},{{1.0,1.0,-1.0}},{{0.0,1.0,-1.0}}};
  request.core.interface_triangles={{{0U,1U,2U}},{{0U,2U,3U}}};
  request.core.tetrahedra={{{0U,1U,2U,4U}},{{1U,2U,4U,5U}},{{2U,4U,5U,6U}},
                           {{0U,2U,3U,4U}},{{2U,3U,4U,6U}},{{3U,4U,6U,7U}}};
  return request;
}
} // namespace

TEST_CASE("two skewed hexahedral chunks preserve a planar frozen surface and pair their seam") {
  tetra::probes::SandwichConfig config;
  config.resolution=4U;
  config.field=tetra::probes::SandwichField::planar;
  const auto report=tetra::probes::run_sandwich_probe(config);
  CHECK(report.monolithic.validation.valid);
  CHECK(report.monolithic.validation.finite_distinct_tetrahedra);
  CHECK(report.monolithic.validation.unique_tetrahedra);
  CHECK(report.monolithic.validation.nondegenerate_background_tetrahedra);
  CHECK(report.monolithic.validation.source_containment);
  CHECK(report.monolithic.validation.no_tetrahedron_overlap);
  CHECK(report.left_chunk.validation.valid);
  CHECK(report.right_chunk.validation.valid);
  CHECK(report.joined_chunks.validation.valid);
  CHECK(report.shared_surface_interface_identical);
  CHECK(report.shared_volume_interface_paired);
  CHECK(report.partition_independent);
  CHECK(report.shared_surface_edges>0U);
  CHECK(report.shared_volume_faces>0U);
  CHECK(report.monolithic.storage.core_tetrahedra>0U);
  CHECK(report.monolithic.storage.transition_tetrahedra>0U);
  CHECK(report.monolithic.storage.explicit_core_bytes>0U);
  CHECK(report.monolithic.storage.implicit_core_descriptor_bytes>0U);
}

TEST_CASE("noisy two-chunk sandwich is deterministic, validated, and reports GPU-shaped stages") {
  tetra::probes::SandwichConfig config;
  config.resolution=6U;
  const auto first=tetra::probes::run_sandwich_probe(config);
  const auto second=tetra::probes::run_sandwich_probe(config);
  CHECK(first.valid);
  CHECK(first.monolithic.validation.frozen_surface_preserved);
  CHECK(first.monolithic.validation.finite_distinct_tetrahedra);
  CHECK(first.monolithic.validation.unique_tetrahedra);
  CHECK(first.monolithic.validation.source_containment);
  CHECK(first.monolithic.validation.no_tetrahedron_overlap);
  CHECK(first.monolithic.validation.sampled_partition);
  CHECK(first.monolithic.quality.minimum_mean_ratio>0.0);
  CHECK(first.monolithic.quality.minimum_dihedral_degrees>0.0);
  CHECK(first.monolithic.storage.transition_bytes>0U);
  CHECK(first.monolithic.timings.field_and_surface_ms>=0.0);
  CHECK(first.monolithic.timings.count_ms>=0.0);
  CHECK(first.monolithic.timings.scan_ms>=0.0);
  CHECK(first.monolithic.timings.emit_ms>=0.0);
  CHECK(first.monolithic.timings.validation_ms>=0.0);
  CHECK(first.monolithic.surface_hash==second.monolithic.surface_hash);
  CHECK(first.monolithic.tetrahedron_hash==second.monolithic.tetrahedron_hash);
  const auto json=tetra::probes::make_sandwich_report_json(first);
  CHECK(json.find("tetra_sandwich_probe/v1")!=std::string::npos);
  CHECK(json.find("bounded-coned-clipped-tetrahedra")!=std::string::npos);
  CHECK(json.find("implicit_core_descriptor_bytes")!=std::string::npos);
  const auto svg=tetra::probes::make_sandwich_svg(config);
  CHECK(svg.find("Two skewed hexahedra")!=std::string::npos);
  CHECK(svg.find("frozen surface triangles")!=std::string::npos);
  CHECK(svg.find("transition tetrahedron edges")!=std::string::npos);
  const auto viewer_data=tetra::probes::make_sandwich_viewer_data(config);
  CHECK(viewer_data.find("dualSurface")!=std::string::npos);
  CHECK(viewer_data.find("\"buildRevision\":\"structured-two-hex-global-core-v5\"")!=std::string::npos);
  CHECK(viewer_data.find("\"dualSource\":{\"algorithm\":\"structured-two-parent-hexahedra-dual-contouring\"")!=std::string::npos);
  CHECK(viewer_data.find("\"parentHexahedra\":2")!=std::string::npos);
  CHECK(viewer_data.find("\"quadCount\":")!=std::string::npos);
  CHECK(viewer_data.find("\"seamQuads\":")!=std::string::npos);
  CHECK(viewer_data.find("\"placement\":\"hermite-mass-point\"")!=std::string::npos);
  CHECK(viewer_data.find("\"parentTetrahedronEdges\":[")!=std::string::npos);
  CHECK(viewer_data.find("\"parentHexahedronEdges\":[")!=std::string::npos);
  CHECK(viewer_data.find("\"structuredGridEdges\":[")!=std::string::npos);
  CHECK(viewer_data.find("\"dualQuadEdges\":[")!=std::string::npos);
  CHECK(viewer_data.find("\"renderDiagonalEdges\":[")!=std::string::npos);
  CHECK(viewer_data.find("\"exactSharedFaceIdentity\":true")!=std::string::npos);
  CHECK(viewer_data.find("\"everyInteriorCrossingIsQuad\":true")!=std::string::npos);
  CHECK(viewer_data.find("\"surfaceValid\":true")!=std::string::npos);
  CHECK(viewer_data.find("\"transitionConstructed\":false")!=std::string::npos);
  CHECK(viewer_data.find("\"completeVolumeValid\":false")!=std::string::npos);
  CHECK(viewer_data.find("\"transitionEdges\":[]")!=std::string::npos);
  CHECK(viewer_data.find("\"implicitCoreEdges\":[]")==std::string::npos);
  CHECK(viewer_data.find("\"dualTriangleCount\":0") == std::string::npos);
}

TEST_CASE("the first resolution and phase corpus keeps the chunk contract") {
  for(const auto resolution : {2U,4U,8U,16U}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;
    const auto report=tetra::probes::run_sandwich_probe(config);
    CHECK(report.valid);
    CHECK(report.partition_independent);
    CHECK(report.monolithic.validation.sampled_gaps==0U);
    CHECK(report.monolithic.validation.sampled_overlaps==0U);
  }

  tetra::probes::SandwichConfig phase_case;
  phase_case.resolution=8U;
  phase_case.phase_x=0.5;
  phase_case.phase_y=0.0001;
  const auto report=tetra::probes::run_sandwich_probe(phase_case);
  CHECK(report.valid);
  // This deliberately awkward phase must stay a valid partition while its
  // diagnostics retain the poor near-cut elements for the quality gate.
  CHECK(report.monolithic.quality.slivers_below_mean_ratio_001>0U);
}

TEST_CASE("dual contouring has deterministic halo ownership and a manifold sheet") {
  for(const auto resolution : {4U,6U,8U}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;
    const auto report=tetra::probes::run_dual_contour_probe(config);
    CHECK(report.valid);
    CHECK(report.monolithic.finite_vertices);
    CHECK(report.monolithic.nondegenerate_triangles);
    CHECK(report.monolithic.unique_triangles);
    CHECK(report.monolithic.manifold_edges);
    CHECK(report.monolithic.consistently_oriented);
    CHECK(report.monolithic.no_strict_triangle_intersections);
    CHECK(report.monolithic.strict_triangle_intersections==0U);
    CHECK(std::isfinite(report.monolithic_surface_quality.minimum_triangle_angle_degrees));
    CHECK(std::isfinite(report.monolithic_surface_quality.minimum_shape_quality));
    CHECK(report.monolithic_surface_quality.maximum_edge_ratio>0.0);
    if(resolution==6U) CHECK(report.monolithic_surface_quality.diagnostic_thresholds_met);
    if(resolution==8U) {
      // The nonlinear field must use its root samples, not endpoint-value
      // interpolation: the same 222-triangle N=8 topology now clears the
      // frozen-surface screen without a topology/refinement change.
      CHECK(report.monolithic_surface_quality.diagnostic_thresholds_met);
      CHECK(report.monolithic_surface_quality.minimum_triangle_angle_degrees>=5.0);
      CHECK(report.monolithic_surface_quality.maximum_edge_ratio<=20.0);
      CHECK(report.monolithic_surface_quality.triangles_below_1_degree==0U);
    }
    CHECK(report.shared_halo_positions_identical);
    CHECK(report.shared_halo_vertices>0U);
    CHECK(report.seam_crossing_triangles>0U);
    CHECK(report.partition_independent);
    CHECK(report.monolithic_hash==report.joined_hash);
  }

  tetra::probes::SandwichConfig planar;
  planar.resolution=4U;
  planar.field=tetra::probes::SandwichField::planar;
  const auto planar_report=tetra::probes::run_dual_contour_probe(planar);
  CHECK(planar_report.valid);
  CHECK(planar_report.monolithic_surface_quality.diagnostic_thresholds_met);

  for(const auto [phase_x,phase_y] : {std::pair{0.0001,0.0001},std::pair{0.5,0.0001},
                                      std::pair{0.73,0.91}}) {
    tetra::probes::SandwichConfig adversarial;
    adversarial.resolution=8U;
    adversarial.phase_x=phase_x;
    adversarial.phase_y=phase_y;
    const auto report=tetra::probes::run_dual_contour_probe(adversarial);
    INFO("phase="<<phase_x<<':'<<phase_y<<" boundaries="<<report.monolithic.boundary_edges
         <<" nonmanifold="<<report.monolithic.nonmanifold_edges
         <<" degenerate="<<report.monolithic.degenerate_triangles
         <<" oriented="<<report.monolithic.consistently_oriented
         <<" left="<<report.left_chunk.valid<<" right="<<report.right_chunk.valid
         <<" seam="<<report.seam_crossing_triangles
         <<" shared="<<report.shared_halo_vertices
         <<" independent="<<report.partition_independent);
    CHECK(report.valid);
    CHECK(report.monolithic.no_strict_triangle_intersections);
    CHECK(report.monolithic.degenerate_triangles==0U);
    CHECK(report.monolithic.nonmanifold_edges==0U);
    CHECK(std::isfinite(report.monolithic_surface_quality.minimum_triangle_angle_degrees));
    CHECK(std::isfinite(report.monolithic_surface_quality.minimum_shape_quality));
  }
}

TEST_CASE("dual contouring owns an exact frozen-boundary prism transition and regenerable core") {
  for(const auto resolution : {4U,6U,8U}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;
    const auto report=tetra::probes::run_dual_contour_probe(config);
    const auto& volume=report.volume;
    INFO("N="<<resolution<<" height field="<<volume.height_field_precondition
         <<" valid="<<volume.valid<<" missing="<<volume.monolithic.missing_frozen_surface_faces
         <<" artificial="<<volume.monolithic.unmatched_non_surface_faces
         <<" overlap="<<volume.monolithic.tetrahedron_overlap_pairs);
    REQUIRE(volume.height_field_precondition);
    CHECK(report.hermite_mass_point_placement);
    CHECK_FALSE(report.qef_placement_qualified);
    CHECK(volume.monolithic.valid);
    CHECK(volume.left_chunk.valid);
    CHECK(volume.right_chunk.valid);
    CHECK(volume.joined_chunks.valid);
    CHECK(volume.monolithic.frozen_surface_preserved);
    CHECK(volume.monolithic.missing_frozen_surface_faces==0U);
    CHECK(volume.monolithic.artificial_boundary_only);
    CHECK(volume.monolithic.no_tetrahedron_overlap);
    CHECK(volume.partition_independent);
    CHECK(volume.monolithic_hash==volume.joined_hash);
    CHECK(volume.storage.transition_tetrahedra>0U);
    CHECK(volume.storage.core_tetrahedra>0U);
    CHECK(volume.storage.implicit_core_descriptor_bytes>0U);
    CHECK(volume.quality.minimum_mean_ratio>0.0);
    CHECK(volume.quality.minimum_dihedral_degrees>0.0);
    if(resolution==8U) {
      // This is only the existing layered collar diagnostic.  It benefits
      // from the corrected frozen sheet but is not evidence that the separate
      // external shell-quality problem is solved.
      CHECK(volume.quality.diagnostic_thresholds_met);
      CHECK(volume.quality.minimum_dihedral_degrees>=5.0);
      CHECK(volume.quality.dihedrals_below_1_degree==0U);
      CHECK(volume.quality.dihedrals_below_5_degrees==0U);
      CHECK(volume.quality.maximum_edge_ratio<=20.0);
    }
  }
  tetra::probes::SandwichConfig planar;
  planar.resolution=6U;
  planar.field=tetra::probes::SandwichField::planar;
  CHECK(tetra::probes::run_dual_contour_probe(planar).volume.valid);

  // Near-zero placements previously exposed DC ownership/placement mistakes.
  // They must qualify the volume itself, not merely leave a valid sheet.
  for(const auto [phase_x,phase_y] : {std::pair{0.0001,0.0001},std::pair{0.5,0.0001},
                                      std::pair{0.73,0.91}}) {
    tetra::probes::SandwichConfig adverse;
    adverse.resolution=8U;adverse.phase_x=phase_x;adverse.phase_y=phase_y;
    const auto report=tetra::probes::run_dual_contour_probe(adverse);
    INFO("near-zero phase="<<phase_x<<':'<<phase_y<<" missing="
         <<report.volume.monolithic.missing_frozen_surface_faces<<" overlaps="
         <<report.volume.monolithic.tetrahedron_overlap_pairs);
    CHECK(report.volume.valid);
    CHECK(report.volume.monolithic.missing_frozen_surface_faces==0U);
    CHECK(report.volume.monolithic.unmatched_non_surface_faces==0U);
    CHECK(report.volume.monolithic.tetrahedron_overlap_pairs==0U);
    CHECK(report.volume.partition_independent);
  }
}

TEST_CASE("dual contour Hermite crossings are bounded, accurate, and canonical") {
  const auto check=[&](tetra::probes::SandwichConfig config) {
    const auto first=tetra::probes::run_hermite_surface_probe(config);
    const auto second=tetra::probes::run_hermite_surface_probe(config);
    const auto& roots=first.monolithic_crossing_quality;
    INFO("N="<<config.resolution<<" phase="<<config.phase_x<<':'<<config.phase_y
         <<" residual="<<roots.maximum_absolute_field_residual
         <<" bracket="<<roots.maximum_bracket_fraction);
    CHECK(roots.deterministic_bounded_policy);
    CHECK(roots.sign_changing_edges>0U);
    CHECK(roots.bracketed_roots+roots.exact_zero_endpoint_roots==roots.sign_changing_edges);
    CHECK(roots.maximum_bisection_iterations<=24U);
    CHECK(roots.maximum_bracket_fraction<=std::ldexp(1.0,-24));
    // With the declared fixed 24-step positional bound, this corpus is more
    // accurate than the 1e-6 field residual used by the surface regression.
    CHECK(roots.maximum_absolute_field_residual<1.0e-6);
    CHECK(first.monolithic_hash==second.monolithic_hash);
    CHECK(first.joined_hash==second.joined_hash);
    CHECK(first.shared_halo_positions_identical);
    CHECK(first.partition_independent);
    CHECK(first.monolithic_surface_quality.diagnostic_thresholds_met);
  };

  tetra::probes::SandwichConfig planar;
  planar.resolution=8U;planar.field=tetra::probes::SandwichField::planar;
  check(planar);

  // At N=30 the fixed planar level set passes exactly through known canonical
  // lattice samples.  This exercises the tie-breaker's endpoint-root path
  // without requiring a special test-only field or a traversal-order cache.
  planar.resolution=30U;
  const auto planar_exact=tetra::probes::sample_hermite_crossings(planar);
  CHECK(planar_exact.deterministic_bounded_policy);
  CHECK(planar_exact.exact_zero_endpoint_roots>0U);
  CHECK(planar_exact.bracketed_roots+planar_exact.exact_zero_endpoint_roots==
        planar_exact.sign_changing_edges);
  CHECK(planar_exact.maximum_absolute_field_residual<1.0e-6);
  CHECK(planar_exact.maximum_bisection_iterations<=24U);
  for(const auto [resolution,phase_x,phase_y] : {
          std::tuple{6U,0.23,0.41},std::tuple{8U,0.23,0.41},
          std::tuple{8U,0.0001,0.0001},std::tuple{8U,0.5,0.0001},
          std::tuple{8U,0.73,0.91},std::tuple{16U,0.0001,0.0001},
          std::tuple{16U,0.5,0.0001}}) {
    tetra::probes::SandwichConfig noisy;
    noisy.resolution=resolution;noisy.phase_x=phase_x;noisy.phase_y=phase_y;
    check(noisy);
  }
}

TEST_CASE("planar DC collar bridges to an exactly regenerable regular tet grid") {
  for(const auto resolution : {4U,6U,8U}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;config.field=tetra::probes::SandwichField::planar;
    const auto bridge=tetra::probes::run_dual_contour_probe(config).regular_grid_bridge;
    INFO("N="<<resolution<<" missing="<<bridge.monolithic.missing_frozen_surface_faces
         <<" overlaps="<<bridge.monolithic.tetrahedron_overlap_pairs);
    REQUIRE(bridge.attempted);
    CHECK(bridge.planar_only);
    CHECK(bridge.valid);
    CHECK(bridge.exact_regular_grid_reconstruction);
    CHECK(bridge.transition_core_interface_paired);
    CHECK(bridge.partition_independent);
    CHECK(bridge.monolithic.frozen_surface_preserved);
    CHECK(bridge.monolithic.missing_frozen_surface_faces==0U);
    CHECK(bridge.monolithic.no_tetrahedron_overlap);
    CHECK(bridge.storage.transition_tetrahedra>0U);
    CHECK(bridge.storage.core_tetrahedra>0U);
    CHECK(bridge.quality.minimum_mean_ratio>0.0);
  }
  tetra::probes::SandwichConfig noisy;
  noisy.resolution=8U;
  const auto bridge=tetra::probes::run_dual_contour_probe(noisy).regular_grid_bridge;
  CHECK(bridge.attempted);
  CHECK(bridge.noisy_diagnostic_attempted);
  CHECK(bridge.noisy_explicitly_unsupported);
  CHECK_FALSE(bridge.valid);
}

TEST_CASE("noisy fixed-grid bridge diagnostic preserves the boundary while exposing its failure") {
  for(const auto resolution : {2U,4U,6U,8U}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;
    const auto bridge=tetra::probes::run_dual_contour_probe(config).regular_grid_bridge;
    INFO("N="<<resolution<<" degenerate="<<bridge.monolithic.degenerate_tetrahedra
         <<" paired="<<bridge.transition_core_interface_paired);
    CHECK(bridge.attempted);
    CHECK(bridge.noisy_diagnostic_attempted);
    CHECK(bridge.noisy_explicitly_unsupported);
    CHECK_FALSE(bridge.valid);
    CHECK(bridge.monolithic.frozen_surface_preserved);
    CHECK(bridge.monolithic.missing_frozen_surface_faces==0U);
    CHECK(bridge.monolithic.artificial_boundary_only);
    CHECK(bridge.monolithic.no_tetrahedron_overlap);
    if(resolution==2U) CHECK_FALSE(bridge.transition_core_interface_paired);
    else {
      CHECK(bridge.coincident_regular_core_vertex_pairs>0U);
      CHECK(bridge.monolithic.degenerate_tetrahedra>0U);
      CHECK(bridge.degenerate_transition_tetrahedra>0U);
    }
  }
  for(const auto [phase_x,phase_y] : {std::pair{0.0001,0.0001},std::pair{0.5,0.0001},
                                      std::pair{0.73,0.91}}) {
    tetra::probes::SandwichConfig config;
    config.resolution=8U;config.phase_x=phase_x;config.phase_y=phase_y;
    const auto bridge=tetra::probes::run_dual_contour_probe(config).regular_grid_bridge;
    INFO("phase="<<phase_x<<':'<<phase_y<<" degenerates="<<bridge.monolithic.degenerate_tetrahedra);
    CHECK(bridge.attempted);
    CHECK(bridge.noisy_diagnostic_attempted);
    CHECK(bridge.noisy_explicitly_unsupported);
    CHECK_FALSE(bridge.valid);
    CHECK(bridge.monolithic.frozen_surface_preserved);
    CHECK(bridge.coincident_regular_core_vertex_pairs>0U);
    CHECK(bridge.monolithic.degenerate_tetrahedra>0U);
  }
}

TEST_CASE("identity-preserving inward grid attachment is rejected for noisy sheets") {
  const auto check_rejected_attachment=[](const tetra::probes::DualGridAttachmentReport& attachment) {
    CHECK(attachment.attempted);
    CHECK(attachment.connector_segments==4U);
    CHECK(attachment.exact_3d_grid_address_reconstruction);
    CHECK(attachment.inner_front_on_material_side);
    CHECK(attachment.partition_independent);
    // This is deliberately a negative control.  Correct Hermite crossings
    // move its frozen sheet, so overlap *counts* are not stable goldens; the
    // construction must remain rejected for the structural reason that
    // independently extruded stepped prisms overlap and share same-side faces.
    CHECK_FALSE(attachment.valid);
    CHECK(attachment.monolithic.frozen_surface_preserved);
    CHECK(attachment.monolithic.missing_frozen_surface_faces==0U);
    CHECK(attachment.monolithic.artificial_boundary_only);
    CHECK_FALSE(attachment.monolithic.no_tetrahedron_overlap);
    CHECK_FALSE(attachment.monolithic.opposing_shared_faces);
    CHECK(attachment.monolithic.tetrahedron_overlap_pairs>0U);
    CHECK(attachment.monolithic.same_side_shared_faces>0U);
    CHECK(attachment.has_first_strict_overlap);
    CHECK(attachment.first_overlap_left!=attachment.first_overlap_right);
  };
  tetra::probes::SandwichConfig small;
  small.resolution=4U;
  const auto small_attachment=tetra::probes::run_dual_contour_probe(small).identity_grid_attachment;
  check_rejected_attachment(small_attachment);

  tetra::probes::SandwichConfig minimum_failure;
  minimum_failure.resolution=6U;
  const auto failure=tetra::probes::run_dual_contour_probe(minimum_failure).identity_grid_attachment;
  check_rejected_attachment(failure);

  tetra::probes::SandwichConfig near_zero;
  near_zero.resolution=8U;near_zero.phase_x=0.0001;near_zero.phase_y=0.0001;
  const auto adverse=tetra::probes::run_dual_contour_probe(near_zero).identity_grid_attachment;
  check_rejected_attachment(adverse);

  tetra::probes::SandwichConfig default_n8;
  default_n8.resolution=8U;
  check_rejected_attachment(tetra::probes::run_dual_contour_probe(default_n8).identity_grid_attachment);
  for(const auto [phase_x,phase_y] : {std::pair{0.5,0.0001},std::pair{0.73,0.91}}) {
    tetra::probes::SandwichConfig phase;
    phase.resolution=8U;phase.phase_x=phase_x;phase.phase_y=phase_y;
    check_rejected_attachment(tetra::probes::run_dual_contour_probe(phase).identity_grid_attachment);
  }
}

TEST_CASE("isolated N6 2:1 step PLC patch rejects a single-centre cone template") {
  tetra::probes::SandwichConfig config;
  config.resolution=6U;
  const auto patch=tetra::probes::run_dual_contour_probe(config).isolated_step_patch;
  REQUIRE(patch.attempted);
  REQUIRE(patch.has_vertical_2_to_1_step);
  CHECK(patch.grid_front_on_material_side);
  CHECK(patch.tetrahedra==8U);
  CHECK(patch.validation.frozen_surface_preserved);
  CHECK(patch.validation.missing_frozen_surface_faces==0U);
  CHECK(patch.validation.artificial_boundary_only);
  CHECK(patch.validation.positive_tetrahedra);
  CHECK_FALSE(patch.validation.no_tetrahedron_overlap);
  CHECK_FALSE(patch.validation.opposing_shared_faces);
  // The selected step and its geometry can change after an accurate Hermite
  // sample update.  The negative control is its nonconforming topology, not
  // a particular number of overlapping tet pairs.
  CHECK(patch.validation.same_side_shared_faces>0U);
  CHECK(patch.validation.tetrahedron_overlap_pairs>0U);
  CHECK_FALSE(patch.valid);
}

TEST_CASE("full first N6 stepped-edge union rejects its split-grid-edge PLC template") {
  tetra::probes::SandwichConfig config;
  config.resolution=6U;
  const auto patch=tetra::probes::run_dual_contour_probe(config).stepped_edge_union_patch;
  REQUIRE(patch.attempted);
  REQUIRE(patch.has_vertical_2_to_1_step);
  CHECK(patch.grid_front_on_material_side);
  CHECK_FALSE(patch.kernel_feasible);
  CHECK(patch.kernel_margin<=0.0);
  CHECK(patch.tetrahedra==0U);
  CHECK_FALSE(patch.valid);
}

TEST_CASE("N6 stepped-edge PLC rejects its self-intersecting prescribed boundary before search") {
  tetra::probes::SandwichConfig config;
  config.resolution=6U;
  const auto patch=tetra::probes::run_dual_contour_probe(config).stepped_edge_union_patch;
  REQUIRE(patch.attempted);
  REQUIRE(patch.has_vertical_2_to_1_step);
  CHECK(patch.plc_self_intersection);
  // The old search inserted candidate-only faces into its obligation map and
  // then called that candidate-family exhaustion. An intersecting PLC is now
  // rejected before the finite template search; neither result is evidence
  // about whether a valid DC-to-grid shell can be tetrahedralized.
  CHECK_FALSE(patch.plc_oracle_attempted);
  CHECK_FALSE(patch.plc_oracle_candidate_family_exhausted);
  CHECK_FALSE(patch.plc_oracle_found_fill);
  CHECK(patch.tetrahedra==0U);
}

TEST_CASE("independent local QEF placement remains diagnostically disqualified") {
  tetra::probes::SandwichConfig adverse;
  adverse.resolution=8U;adverse.phase_x=0.0001;adverse.phase_y=0.0001;
  const auto qef=tetra::probes::run_dual_contour_qef_diagnostic(adverse);
  CHECK_FALSE(qef.qef_surface.valid);
  CHECK_FALSE(qef.qef_surface.no_strict_triangle_intersections);
  CHECK(qef.qef_surface.strict_triangle_intersections>0U);
  REQUIRE(qef.qef_volume_attempted);
  CHECK_FALSE(qef.qef_volume_qualified);
  CHECK_FALSE(qef.qef_volume.no_tetrahedron_overlap);
  CHECK(qef.qef_volume.tetrahedron_overlap_pairs>0U);
}

TEST_CASE("DC chunk interface precondition owns whole crossing triangles and selects core independently") {
  // These are deliberately noisy rather than a planar proxy: a DC triangle
  // crosses the original hexahedral seam in every case.  The test only passes
  // if that triangle is emitted once by its canonical owner and both requests
  // independently select the same joined regular core as the monolithic
  // oracle.  It is not an independent shell-meshing result.
  for(const auto resolution : {4U,6U,8U}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;
    const auto report=tetra::probes::run_dual_chunk_interface_probe(config);
    INFO("N="<<resolution<<" left="<<report.left_owned_triangles<<" right="<<report.right_owned_triangles
         <<" crossing="<<report.crossing_owned_by_left<<" seam_edges="<<report.canonical_seam_edges
         <<" core="<<report.left_retained_core_tetrahedra<<':'<<report.right_retained_core_tetrahedra
         <<" paired="<<report.paired_core_interface_faces);
    CHECK(report.halo_positions_identical);
    CHECK(report.chunk_results_generated_independently);
    CHECK(report.whole_triangle_ownership);
    CHECK(report.canonical_surface_partition);
    CHECK(report.local_source_matches_monolithic);
    CHECK(report.bounded_local_source_work);
    CHECK(report.assembly_order_and_permutation_independent);
    CHECK(report.canonical_seam_edge_ids);
    CHECK(report.automatic_retained_core_selection);
    CHECK(report.retained_core_partition_independent);
    CHECK(report.retained_core_interface_paired);
    CHECK(report.crossing_owned_by_left>0U);
    CHECK(report.canonical_seam_edges>0U);
    CHECK(report.paired_core_interface_faces>0U);
    CHECK(report.monolithic_surface_hash==report.joined_owned_surface_hash);
    CHECK(report.monolithic_core_hash==report.joined_core_hash);
    CHECK(report.left_requested_cells>0U);
    CHECK(report.left_halo_cells>0U);
    CHECK(report.left_seam_dependency_cells>0U);
    CHECK(report.right_seam_dependency_cells>0U);
    CHECK_FALSE(report.independent_shell_meshing_completed);
  }
}

TEST_CASE("DC chunk source requests are bounded and exactly match monolithic output") {
  // This tests the source producer used by the external TetGen oracle, not
  // merely its post-hoc whole-triangle ownership.  Every current default and
  // phase fixture is compared with a monolithic control.  The same left
  // request is then repeated while unrelated positive-x world cells grow to
  // eight times the two-chunk span; its requested/halo/seam-support work is
  // required to stay fixed.
  const auto report=tetra::probes::run_dual_chunk_locality_probe();
  INFO(report.conclusion<<" fixtures="<<report.fixture_count
       <<" remote="<<report.remote_domain_cases<<" requested="<<report.maximum_requested_cells
       <<" halo="<<report.maximum_halo_cells<<" seam="<<report.maximum_seam_dependency_cells
       <<" peak="<<report.maximum_temporary_cells);
  CHECK(report.fixture_count==6U);
  CHECK(report.remote_domain_cases==18U);
  CHECK(report.local_outputs_match_monolithic);
  CHECK(report.reverse_request_order_independent);
  CHECK(report.remote_domain_growth_bounded);
  CHECK(report.maximum_requested_cells>0U);
  CHECK(report.maximum_halo_cells>0U);
  CHECK(report.maximum_seam_dependency_cells>0U);
  CHECK(report.maximum_temporary_cells>report.maximum_requested_cells);
}

TEST_CASE("frozen noisy DC surface export is deterministic and surface-only") {
  tetra::probes::SandwichConfig config;
  config.resolution=4U;
  config.amplitude=0.24;
  config.frequency=2.25;
  const auto first=tetra::probes::extract_frozen_dual_contour_surface(config);
  const auto repeat=tetra::probes::extract_frozen_dual_contour_surface(config);
  CHECK(first.validation.valid);
  CHECK_FALSE(first.vertices.empty());
  CHECK_FALSE(first.triangles.empty());
  CHECK(first.stable_vertex_ids==repeat.stable_vertex_ids);
  CHECK(first.vertices==repeat.vertices);
  CHECK(first.triangles==repeat.triangles);
  CHECK_FALSE(first.boundary_edges.empty());
  CHECK(first.boundary_edges==repeat.boundary_edges);
  std::map<std::uint32_t,unsigned int> boundary_in,boundary_out;
  for(const auto edge:first.boundary_edges){++boundary_out[edge[0]];++boundary_in[edge[1]];}
  CHECK(boundary_in==boundary_out);
  CHECK(std::all_of(boundary_in.begin(),boundary_in.end(),[](const auto& entry){return entry.second==1U;}));

  const auto core=tetra::probes::extract_selected_regular_core(config);
  const auto repeated_core=tetra::probes::extract_selected_regular_core(config);
  CHECK_FALSE(core.vertices.empty());
  CHECK_FALSE(core.tetrahedra.empty());
  CHECK(core.stable_vertex_ids==repeated_core.stable_vertex_ids);
  CHECK(core.vertices==repeated_core.vertices);
  CHECK(core.tetrahedra==repeated_core.tetrahedra);
}

TEST_CASE("independent regular core spans the chunk and exposes only its top interface") {
  tetra::probes::SandwichConfig first_config;
  first_config.resolution=8U;
  first_config.amplitude=0.08;
  first_config.frequency=1.25;
  first_config.phase_x=-0.7;
  first_config.phase_y=1.1;
  auto second_config=first_config;
  second_config.amplitude=0.31;
  second_config.frequency=3.75;
  second_config.phase_x=4.2;
  second_config.phase_y=-2.6;

  const auto first=tetra::probes::extract_independent_regular_core(first_config);
  const auto second=tetra::probes::extract_independent_regular_core(second_config);
  CHECK(first.stable_vertex_ids==second.stable_vertex_ids);
  CHECK(first.vertices==second.vertices);
  CHECK(first.tetrahedra==second.tetrahedra);
  CHECK(first.interface_triangles==second.interface_triangles);
  CHECK(first.tetrahedra.size()==2304U);
  CHECK(first.interface_triangles.size()==256U);

  std::map<std::array<std::uint32_t,3>,unsigned int> face_uses;
  for(const auto tet:first.tetrahedra) for(std::size_t omitted=0U;omitted<4U;++omitted) {
    std::array<std::uint32_t,3> face{};
    std::size_t cursor{};
    for(std::size_t vertex=0U;vertex<4U;++vertex) if(vertex!=omitted) face[cursor++]=tet[vertex];
    std::sort(face.begin(),face.end());
    ++face_uses[face];
  }
  for(const auto face:first.interface_triangles) {
    CHECK(face_uses[face]==1U);
    for(const auto vertex:face)CHECK(first.stable_vertex_ids[vertex]%(first_config.resolution+1U)==3U);
  }


  const auto surface=tetra::probes::extract_frozen_dual_contour_surface(first_config);
  const auto preflight=tetra::probes::preflight_independent_core_interface(surface,first);
  CHECK(preflight.valid);
  CHECK(preflight.uncovered_surface_vertices==0U);
  CHECK(preflight.minimum_normal_clearance>0.1);
  CHECK(preflight.surface_projected_area>0.0);
  CHECK(preflight.interface_projected_area>preflight.surface_projected_area);

  auto moved_surface=surface;
  auto moved_core=first;
  const auto move=[](std::array<double,3>& p) {
    const auto x=p[0],y=p[1],z=p[2];
    p={{-y+2.5,z-1.75,-x+0.625}};
  };
  for(auto& p:moved_surface.vertices)move(p);
  for(auto& p:moved_core.vertices)move(p);
  const auto moved=tetra::probes::preflight_independent_core_interface(moved_surface,moved_core);
  CHECK(moved.valid);
  CHECK(moved.uncovered_surface_vertices==0U);
  CHECK(moved.minimum_normal_clearance==doctest::Approx(preflight.minimum_normal_clearance).epsilon(1e-10));
  CHECK(moved.surface_projected_area==doctest::Approx(preflight.surface_projected_area).epsilon(1e-10));
  CHECK(moved.interface_projected_area==doctest::Approx(preflight.interface_projected_area).epsilon(1e-10));

  const auto overlay=tetra::probes::construct_surface_grid_overlay(surface,first);
  INFO("overlay failure="<<static_cast<unsigned int>(overlay.failure)
       <<" vertices="<<overlay.vertices.size()<<" triangles="<<overlay.triangles.size()
       <<" area="<<overlay.overlay_projected_area<<'/'<<overlay.surface_projected_area);
  CHECK(overlay.accepted());
  CHECK_FALSE(overlay.vertices.empty());
  CHECK(overlay.triangles.size()>surface.triangles.size());
  CHECK(overlay.overlay_projected_area==doctest::Approx(overlay.surface_projected_area).epsilon(1e-9));
  CHECK(overlay.minimum_normal_separation>0.1);

  auto reordered_surface=surface;
  auto reordered_core=first;
  std::reverse(reordered_surface.triangles.begin(),reordered_surface.triangles.end());
  std::reverse(reordered_core.interface_triangles.begin(),reordered_core.interface_triangles.end());
  const auto reordered=tetra::probes::construct_surface_grid_overlay(reordered_surface,reordered_core);
  REQUIRE(reordered.accepted());
  CHECK(reordered.triangles==overlay.triangles);
  const auto ids=[](const auto& value){std::vector<std::uint64_t> result;for(const auto& vertex:value.vertices)result.push_back(vertex.stable_id);return result;};
  CHECK(ids(reordered)==ids(overlay));

  const auto moved_overlay=tetra::probes::construct_surface_grid_overlay(moved_surface,moved_core);
  REQUIRE(moved_overlay.accepted());
  CHECK(moved_overlay.triangles==overlay.triangles);
  CHECK(ids(moved_overlay)==ids(overlay));
  CHECK(moved_overlay.overlay_projected_area==doctest::Approx(overlay.overlay_projected_area).epsilon(1e-9));
  CHECK(moved_overlay.minimum_normal_separation==doctest::Approx(overlay.minimum_normal_separation).epsilon(1e-9));

  const auto transition=tetra::probes::construct_surface_grid_transition_layer(overlay);
  INFO("transition failure="<<static_cast<unsigned int>(transition.failure)
       <<" tetrahedra="<<transition.tetrahedra.size()
       <<" dihedrals="<<transition.quality.minimum_dihedral_degrees
       <<':'<<transition.quality.maximum_dihedral_degrees);
  CHECK(overlay.minimum_surface_triangle_angle_degrees<1.0);
  CHECK_FALSE(transition.accepted());
  CHECK(transition.failure==tetra::probes::SurfaceGridTransitionFailure::quality_refused);
  CHECK_FALSE(transition.quality.diagnostic_thresholds_met);
  CHECK(transition.tetrahedra.size()==overlay.triangles.size()*3U);
  CHECK(transition.surface_triangles.size()==overlay.triangles.size());
  CHECK(transition.interface_triangles.size()==overlay.triangles.size());
}

TEST_CASE("direct N8 overlay prisms are conforming but consistently quality refused") {
  for(const auto phase:std::array<std::array<double,2>,3>{{
      {{0.23,0.41}},{{0.63,0.17}},{{1.20,-0.40}}}}) {
    tetra::probes::SandwichConfig config;
    config.resolution=8U;
    config.phase_x=phase[0];
    config.phase_y=phase[1];
    const auto surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    const auto core=tetra::probes::extract_independent_regular_core(config);
    const auto overlay=tetra::probes::construct_surface_grid_overlay(surface,core);
    INFO("phase="<<phase[0]<<':'<<phase[1]
         <<" parents="<<surface.triangles.size()
         <<" overlay="<<overlay.triangles.size());
    REQUIRE(overlay.accepted());
    CHECK(overlay.overlay_projected_area==doctest::Approx(overlay.surface_projected_area).epsilon(1e-9));
    const auto transition=tetra::probes::construct_surface_grid_transition_layer(overlay);
    CHECK(transition.failure==tetra::probes::SurfaceGridTransitionFailure::quality_refused);
    CHECK(transition.tetrahedra.size()==overlay.triangles.size()*3U);
    CHECK(transition.quality.minimum_dihedral_degrees<1.0);
    CHECK(transition.quality.maximum_dihedral_degrees>179.0);
  }
}

TEST_CASE("shared-lattice zipper refines mismatched fronts and retains the deep core") {
  const auto request=mismatched_square_zipper_request();
  const auto result=tetra::probes::construct_shared_lattice_zipper(request);
  INFO("failure="<<static_cast<unsigned int>(result.failure)
       <<" tets="<<result.tetrahedra.size()<<" overlap="<<result.overlap_pairs
       <<" volume="<<result.tetrahedral_volume<<':'<<result.boundary_volume
       <<" dihedrals="<<result.quality.minimum_dihedral_degrees<<':'<<result.quality.maximum_dihedral_degrees);
  CHECK(result.geometry_valid);
  CHECK(result.exact_surface_preserved);
  CHECK(result.exact_interface_preserved);
  CHECK(result.positive_tetrahedra);
  CHECK(result.face_incidence_valid);
  CHECK(result.no_strict_overlap);
  CHECK(result.exact_volume_agreement);
  CHECK(result.transition_tetrahedra==12U);
  CHECK(result.refined_core_interface_tetrahedra==4U);
  CHECK(result.retained_core_tetrahedra==4U);
  CHECK(result.surface_triangles.size()==4U);
  CHECK(result.interface_triangles.size()==4U);
  CHECK_FALSE(result.side_triangles.empty());
  CHECK(result.s4_passed);
  CHECK(result.accepted());

  auto reordered=request;
  std::reverse(reordered.surface.triangles.begin(),reordered.surface.triangles.end());
  std::reverse(reordered.core.tetrahedra.begin(),reordered.core.tetrahedra.end());
  std::reverse(reordered.core.interface_triangles.begin(),reordered.core.interface_triangles.end());
  const auto repeated=tetra::probes::construct_shared_lattice_zipper(reordered);
  CHECK(repeated.failure==result.failure);
  CHECK(repeated.tetrahedra==result.tetrahedra);
  CHECK(repeated.surface_triangles==result.surface_triangles);
  CHECK(repeated.interface_triangles==result.interface_triangles);

  auto transformed=request;
  const auto move=[](std::array<double,3>& p) {
    const auto x=p[0],y=p[1],z=p[2];
    p={{-y+3.0,z-2.0,-x+0.5}};
  };
  for(auto& point:transformed.surface.vertices)move(point);
  for(auto& point:transformed.core.vertices)move(point);
  const auto rigid=tetra::probes::construct_shared_lattice_zipper(transformed);
  CHECK(rigid.accepted());
  CHECK(rigid.tetrahedra==result.tetrahedra);
  CHECK(rigid.surface_triangles==result.surface_triangles);
  CHECK(rigid.interface_triangles==result.interface_triangles);
  CHECK(rigid.tetrahedral_volume==doctest::Approx(result.tetrahedral_volume).epsilon(1e-10));
  CHECK(rigid.quality.minimum_dihedral_degrees==doctest::Approx(result.quality.minimum_dihedral_degrees).epsilon(1e-10));
}

TEST_CASE("shared-lattice zipper fails transactionally at declared resource limits") {
  auto request=mismatched_square_zipper_request();
  request.limits.maximum_overlay_triangles=1U;
  const auto result=tetra::probes::construct_shared_lattice_zipper(request);
  CHECK(result.failure==tetra::probes::SharedLatticeZipperFailure::resource_limit);
  CHECK(result.tetrahedra.empty());
}

TEST_CASE("two-front side-wall zipper accepts unequal boundary sampling deterministically") {
  using Vertex=tetra::probes::SharedLatticeLoopVertex;
  std::vector<Vertex> surface{{1U,{{0,0,1}}},{2U,{{1,0,1}}},{3U,{{1,1,1}}},{4U,{{0,1,1}}}};
  std::vector<Vertex> interface{{11U,{{0,0,0}}},{12U,{{.5,0,0}}},{13U,{{1,0,0}}},{14U,{{1,.5,0}}},
                                {15U,{{1,1,0}}},{16U,{{.5,1,0}}},{17U,{{0,1,0}}},{18U,{{0,.5,0}}}};
  const auto first=tetra::probes::construct_shared_lattice_side_wall(surface,interface);
  REQUIRE(first.accepted());
  CHECK(first.triangles.size()==surface.size()+interface.size());
  CHECK(first.dynamic_programming_states==(surface.size()+1U)*(interface.size()+1U));
  CHECK(first.cost>0.0);

  std::rotate(surface.begin(),surface.begin()+2,surface.end());
  std::rotate(interface.begin(),interface.begin()+3,interface.end());
  const auto rotated=tetra::probes::construct_shared_lattice_side_wall(surface,interface);
  REQUIRE(rotated.accepted());
  CHECK(rotated.triangles==first.triangles);
  CHECK(rotated.cost==doctest::Approx(first.cost).epsilon(1e-12));

  const auto move=[](Vertex& vertex) {
    const auto x=vertex.position[0],y=vertex.position[1],z=vertex.position[2];
    vertex.position={{-z+4.0,x-3.0,y+2.0}};
  };
  for(auto& vertex:surface)move(vertex);
  for(auto& vertex:interface)move(vertex);
  const auto transformed=tetra::probes::construct_shared_lattice_side_wall(surface,interface);
  REQUIRE(transformed.accepted());
  CHECK(transformed.triangles==first.triangles);
  CHECK(transformed.cost==doctest::Approx(first.cost).epsilon(1e-12));
  const auto bounded=tetra::probes::construct_shared_lattice_side_wall(surface,interface,11U);
  CHECK(bounded.failure==tetra::probes::SharedLatticeSideWallFailure::resource_limit);
  CHECK(bounded.triangles.empty());
}

TEST_CASE("full terrain gap reports whether one global star patch is sufficient") {
  for(const auto [resolution,field] : {
          std::pair{6U,tetra::probes::SandwichField::planar},
          std::pair{6U,tetra::probes::SandwichField::perlin_height},
          std::pair{8U,tetra::probes::SandwichField::perlin_height}}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;config.field=field;
    const auto surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    const auto core=tetra::probes::extract_independent_regular_core(config);
    const auto result=tetra::probes::probe_shared_lattice_star_gap(surface,core);
    INFO("N="<<resolution<<" field="<<static_cast<unsigned int>(field)
         <<" failure="<<static_cast<unsigned int>(result.failure)
         <<" loops="<<result.surface_loop_vertices<<':'<<result.interface_loop_vertices
         <<" wall="<<result.side_triangles<<" faces="<<result.boundary_triangles
         <<" margin="<<result.kernel_margin<<" tets="<<result.tetrahedra
         <<" overlap="<<result.overlap_pairs
         <<" quality="<<result.quality.minimum_dihedral_degrees<<':'<<result.quality.maximum_dihedral_degrees);
    CHECK(result.closed_boundary);
    CHECK(result.surface_loop_vertices>0U);
    CHECK(result.interface_loop_vertices>0U);
    CHECK(result.side_triangles==result.surface_loop_vertices+result.interface_loop_vertices);
    if(field==tetra::probes::SandwichField::perlin_height) {
      CHECK_FALSE(result.star_shaped);
      CHECK(result.failure==tetra::probes::SharedLatticeStarGapFailure::non_star_gap);
    } else {
      CHECK(result.accepted());
      CHECK(result.star_shaped);
      CHECK(result.positive_tetrahedra);
      CHECK(result.no_strict_overlap);
      CHECK(result.overlap_pairs==0U);
      CHECK(result.exact_volume_agreement);
      CHECK(result.tetrahedra==result.boundary_triangles);
      CHECK(result.quality.diagnostic_thresholds_met);
    }
  }
}

TEST_CASE("shared-lattice ownership retains exact DC edge and Freudenthal square provenance") {
  for(const auto [resolution,field] : {
          std::pair{6U,tetra::probes::SandwichField::planar},
          std::pair{6U,tetra::probes::SandwichField::perlin_height},
          std::pair{8U,tetra::probes::SandwichField::perlin_height}}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;config.field=field;
    const auto surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    const auto core=tetra::probes::extract_independent_regular_core(config);
    const auto report=tetra::probes::inspect_shared_lattice_ownership(surface,core);
    INFO("N="<<resolution<<" field="<<static_cast<unsigned int>(field)
         <<" dc="<<report.dc_primal_edges
         <<" vertical="<<report.dc_vertical_edges
         <<" horizontal="<<report.dc_horizontal_edges
         <<" core="<<report.core_interface_squares
         <<" paired="<<report.paired_vertical_squares
         <<" ring="<<report.unpaired_core_squares);
    CHECK(report.accepted());
    CHECK(report.resolution==resolution);
    CHECK(report.exact_two_triangles_per_dc_quad);
    CHECK(report.exact_two_triangles_per_core_square);
    CHECK(report.deterministic_local_ownership);
    CHECK(report.paired_vertical_squares==report.dc_vertical_edges);
    CHECK(report.core_interface_squares==static_cast<std::size_t>(2U*resolution)*resolution);
    CHECK(report.unpaired_core_squares==3U*resolution-1U);
    if(field==tetra::probes::SandwichField::planar)CHECK(report.dc_horizontal_edges==0U);
    else CHECK(report.dc_horizontal_edges>0U);
  }
}

TEST_CASE("lattice-owned prism advances include collapsed sparse step operations") {
  for(const auto [resolution,field] : {
          std::pair{6U,tetra::probes::SandwichField::planar},
          std::pair{6U,tetra::probes::SandwichField::perlin_height},
          std::pair{8U,tetra::probes::SandwichField::perlin_height}}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;config.field=field;
    const auto surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    const auto core=tetra::probes::extract_shared_lattice_regular_core(config);
    const auto ownership=tetra::probes::inspect_shared_lattice_ownership(surface,core);
    const auto result=tetra::probes::probe_shared_lattice_local_transition(surface,core);
    INFO("N="<<resolution<<" field="<<static_cast<unsigned int>(field)
         <<" quads="<<result.paired_quads<<'+'<<result.step_quads
         <<" ring="<<result.perimeter_squares<<" tets="<<result.tetrahedra
         <<" collapsed="<<result.combinatorially_collapsed_tetrahedra
         <<" open="<<result.open_boundary_faces
         <<" overlap="<<result.overlap_pairs
         <<" quality="<<result.quality.minimum_dihedral_degrees<<':'
         <<result.quality.maximum_dihedral_degrees
         <<" mean="<<result.quality.minimum_mean_ratio
         <<" edge="<<result.quality.maximum_edge_ratio);
    REQUIRE(ownership.accepted());
    CHECK(result.accepted());
    CHECK(result.paired_quads==ownership.dc_vertical_edges);
    CHECK(result.step_quads==ownership.dc_horizontal_edges);
    CHECK(ownership.unpaired_core_squares==0U);
    CHECK(result.perimeter_squares==0U);
    CHECK(result.surface_triangles==2U*(result.paired_quads+result.step_quads));
    CHECK(result.interface_triangles==2U*result.paired_quads);
    CHECK(result.perimeter_tetrahedra==0U);
    CHECK(result.tetrahedra==6U*result.paired_quads+3U*result.step_quads);
    CHECK(result.combinatorially_collapsed_tetrahedra==3U*result.step_quads);
    CHECK(result.open_boundary_faces>0U);
    CHECK(result.exact_surface_subset_preserved);
    CHECK(result.exact_core_faces_preserved);
    CHECK(result.positive_tetrahedra);
    CHECK(result.valid_face_incidence);
    CHECK(result.no_strict_overlap);
    CHECK(result.overlap_pairs==0U);
    CHECK(result.quality.minimum_mean_ratio>0.0);
  }
}

TEST_CASE("lattice-owned transition attaches to the unchanged matched Freudenthal core") {
  for(const auto resolution:{6U,8U}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;
    tetra::probes::SharedLatticeZipperRequest request;
    request.surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    request.core=tetra::probes::extract_shared_lattice_regular_core(config);
    const auto result=tetra::probes::construct_shared_lattice_zipper(request);
    INFO("N="<<resolution<<" failure="<<static_cast<unsigned int>(result.failure)
         <<" transition="<<result.transition_tetrahedra
         <<" core="<<result.retained_core_tetrahedra
         <<" boundary="<<result.boundary_faces
         <<" side="<<result.side_triangles.size()
         <<" overlap="<<result.overlap_pairs
         <<" volume="<<result.tetrahedral_volume<<':'<<result.boundary_volume
         <<" quality="<<result.quality.minimum_dihedral_degrees<<':'
         <<result.quality.maximum_dihedral_degrees
         <<" mean="<<result.quality.minimum_mean_ratio
         <<" edge="<<result.quality.maximum_edge_ratio);
    CHECK(result.accepted());
    CHECK(result.geometry_valid);
    CHECK(result.s4_passed);
    CHECK(result.exact_surface_preserved);
    CHECK(result.exact_interface_preserved);
    CHECK(result.positive_tetrahedra);
    CHECK(result.face_incidence_valid);
    CHECK(result.no_strict_overlap);
    CHECK(result.overlap_pairs==0U);
    CHECK(result.exact_volume_agreement);
    CHECK(result.transition_tetrahedra>0U);
    CHECK(result.refined_core_interface_tetrahedra==0U);
    CHECK(result.retained_core_tetrahedra==request.core.tetrahedra.size());
    CHECK(result.surface_triangles.size()==request.surface.triangles.size());
    CHECK(result.interface_triangles.size()==request.core.interface_triangles.size());
    CHECK_FALSE(result.side_triangles.empty());
  }
}

TEST_CASE("shared-lattice retained core is geometrically Cartesian, not hexahedrally warped") {
  for(const auto resolution:{6U,8U}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;
    const auto core=tetra::probes::extract_shared_lattice_regular_core(config);
    REQUIRE_FALSE(core.vertices.empty());
    const auto side=static_cast<std::uint64_t>(resolution)+1U;
    for(std::size_t index=0U;index<core.vertices.size();++index) {
      auto id=core.stable_vertex_ids[index];
      const auto k=static_cast<unsigned int>(id%side);id/=side;
      const auto j=static_cast<unsigned int>(id%side);id/=side;
      const auto i=static_cast<unsigned int>(id);
      const std::array<double,3> expected{{
          -1.0+static_cast<double>(i)/resolution,
          -1.0+2.0*static_cast<double>(j)/resolution,
          -1.0+2.0*static_cast<double>(k)/resolution}};
      CHECK(core.vertices[index][0]==doctest::Approx(expected[0]).epsilon(1e-14));
      CHECK(core.vertices[index][1]==doctest::Approx(expected[1]).epsilon(1e-14));
      CHECK(core.vertices[index][2]==doctest::Approx(expected[2]).epsilon(1e-14));
    }
  }
}

TEST_CASE("lattice-owned zipper is record-order and rigid-transform deterministic") {
  tetra::probes::SandwichConfig config;config.resolution=8U;
  tetra::probes::SharedLatticeZipperRequest request;
  request.surface=tetra::probes::extract_frozen_dual_contour_surface(config);
  request.core=tetra::probes::extract_shared_lattice_regular_core(config);
  const auto baseline=tetra::probes::construct_shared_lattice_zipper(request);
  REQUIRE(baseline.accepted());

  auto reordered=request;
  std::reverse(reordered.surface.triangles.begin(),reordered.surface.triangles.end());
  std::reverse(reordered.surface.triangle_primal_edge_owners.begin(),
               reordered.surface.triangle_primal_edge_owners.end());
  std::reverse(reordered.core.interface_triangles.begin(),reordered.core.interface_triangles.end());
  std::reverse(reordered.core.interface_square_owners.begin(),reordered.core.interface_square_owners.end());
  std::reverse(reordered.core.tetrahedra.begin(),reordered.core.tetrahedra.end());
  const auto permuted=tetra::probes::construct_shared_lattice_zipper(reordered);
  REQUIRE(permuted.accepted());
  CHECK(permuted.tetrahedra==baseline.tetrahedra);
  CHECK(permuted.surface_triangles==baseline.surface_triangles);
  CHECK(permuted.interface_triangles==baseline.interface_triangles);
  CHECK(permuted.side_triangles==baseline.side_triangles);

  auto transformed=request;
  const auto move=[](std::array<double,3>& p) {
    const auto x=p[0],y=p[1],z=p[2];p={{-y+3.0,z-2.0,-x+0.5}};
  };
  for(auto& p:transformed.surface.vertices)move(p);
  for(auto& p:transformed.core.vertices)move(p);
  const auto rigid=tetra::probes::construct_shared_lattice_zipper(transformed);
  REQUIRE(rigid.accepted());
  CHECK(rigid.tetrahedra==baseline.tetrahedra);
  CHECK(rigid.surface_triangles==baseline.surface_triangles);
  CHECK(rigid.interface_triangles==baseline.interface_triangles);
  CHECK(rigid.side_triangles==baseline.side_triangles);
  CHECK(rigid.tetrahedral_volume==doctest::Approx(baseline.tetrahedral_volume).epsilon(1e-10));
  CHECK(rigid.quality.minimum_dihedral_degrees==
        doctest::Approx(baseline.quality.minimum_dihedral_degrees).epsilon(1e-10));

  auto bounded=request;bounded.limits.maximum_output_tetrahedra=10U;
  const auto refused=tetra::probes::construct_shared_lattice_zipper(bounded);
  CHECK(refused.failure==tetra::probes::SharedLatticeZipperFailure::resource_limit);
  CHECK(refused.tetrahedra.empty());
}

TEST_CASE("lattice-owned zipper accepts the required noisy phase corpus") {
  for(const auto resolution:{6U,8U})for(const auto phase:std::array<std::array<double,2>,3>{
      {{{0.23,0.41}},{{0.63,0.17}},{{1.20,-0.40}}}}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;
    config.phase_x=phase[0];config.phase_y=phase[1];
    tetra::probes::SharedLatticeZipperRequest request;
    request.surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    request.core=tetra::probes::extract_shared_lattice_regular_core(config);
    const auto result=tetra::probes::construct_shared_lattice_zipper(request);
    INFO("N="<<resolution<<" phase="<<phase[0]<<':'<<phase[1]
         <<" failure="<<static_cast<unsigned int>(result.failure)
         <<" transition="<<result.transition_tetrahedra
         <<" quality="<<result.quality.minimum_dihedral_degrees<<':'
         <<result.quality.maximum_dihedral_degrees
         <<" mean="<<result.quality.minimum_mean_ratio
         <<" edge="<<result.quality.maximum_edge_ratio);
    CHECK(result.accepted());
    CHECK(result.geometry_valid);
    CHECK(result.s4_passed);
    CHECK(result.exact_surface_preserved);
    CHECK(result.exact_interface_preserved);
    CHECK(result.no_strict_overlap);
    CHECK(result.exact_volume_agreement);
  }
}

TEST_CASE("canonical owners reproduce byte-identical adjacent chunk seams") {
  for(const auto [resolution,field] : {
          std::pair{6U,tetra::probes::SandwichField::planar},
          std::pair{6U,tetra::probes::SandwichField::perlin_height},
          std::pair{8U,tetra::probes::SandwichField::perlin_height}}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;config.field=field;
    const auto report=tetra::probes::validate_shared_lattice_adjacent_chunks(config);
    INFO("N="<<resolution<<" field="<<static_cast<unsigned int>(field)
         <<" accepted="<<report.left_accepted<<':'<<report.right_accepted
         <<" tets="<<report.left_tetrahedra<<':'<<report.right_tetrahedra
         <<" seam="<<report.shared_boundary_faces
         <<" reproduced="<<report.monolithic_topology_reproduced);
    CHECK(report.valid);
    CHECK(report.left_accepted);
    CHECK(report.right_accepted);
    CHECK(report.byte_identical_shared_faces);
    CHECK(report.shared_boundary_faces>0U);
    CHECK(report.monolithic_topology_reproduced);
    CHECK(report.left_tetrahedra+report.right_tetrahedra>0U);
  }
}

TEST_CASE("arbitrary geometric tile coning remains a rejected diagnostic") {
  for(const auto resolution:{6U,8U})for(const auto divisions:{2U,4U,8U}) {
    tetra::probes::SandwichConfig config;config.resolution=resolution;
    const auto surface=tetra::probes::extract_frozen_dual_contour_surface(config);
    const auto core=tetra::probes::extract_independent_regular_core(config);
    const auto result=tetra::probes::probe_shared_lattice_star_partition(surface,core,divisions);
    INFO("N="<<resolution<<" divisions="<<divisions
         <<" failure="<<static_cast<unsigned int>(result.failure)
         <<" patches="<<result.patches<<" star="<<result.all_star_shaped
         <<" geometry="<<result.all_geometry_valid<<" seams="<<result.front_seams_identical
         <<" cut="<<result.surface_cut_edges<<':'<<result.interface_cut_edges
         <<" margin="<<result.minimum_kernel_margin
         <<" quality="<<result.quality.minimum_dihedral_degrees<<':'<<result.quality.maximum_dihedral_degrees
         <<" mean="<<result.quality.minimum_mean_ratio<<" edge="<<result.quality.maximum_edge_ratio);
    CHECK(result.patches==divisions*divisions);
    CHECK(result.surface_cut_edges>0U);
    CHECK(result.interface_cut_edges>0U);
    CHECK(result.all_closed);
    CHECK_FALSE(result.all_geometry_valid);
    CHECK_FALSE(result.s4_passed);
    CHECK_FALSE(result.accepted());
    CHECK((result.failure==tetra::probes::SharedLatticePartitionFailure::patch_refused||
           result.failure==tetra::probes::SharedLatticePartitionFailure::seam_mismatch));
  }
}

TEST_CASE("sandwich core uses the implicit pre-atmosphere BCC hierarchy") {
  tetra::probes::SandwichConfig planar;
  planar.resolution=8U;
  planar.field=tetra::probes::SandwichField::planar;
  const auto first=tetra::probes::extract_implicit_bcc_hierarchy_core(planar);
  const auto repeated=tetra::probes::extract_implicit_bcc_hierarchy_core(planar);
  CHECK(first.red_depth==3U);
  CHECK_FALSE(first.logical_owners.empty());
  CHECK_FALSE(first.interface_faces.empty());
  CHECK(first.interface_face_owners.size()==first.interface_faces.size());
  CHECK(first.logical_owners==repeated.logical_owners);
  CHECK(first.interface_faces==repeated.interface_faces);
  CHECK(std::ranges::is_sorted(first.logical_owners));
  CHECK(std::adjacent_find(first.logical_owners.begin(),first.logical_owners.end())==
        first.logical_owners.end());

  const auto six_volume=[](const tetra::WorldTetrahedronGeometry& g) {
    const auto subtract=[](tetra::Vec3 a,tetra::Vec3 b) {
      return tetra::Vec3{a.x-b.x,a.y-b.y,a.z-b.z};
    };
    const auto cross=[](tetra::Vec3 a,tetra::Vec3 b) {
      return tetra::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,
                         a.x*b.y-a.y*b.x};
    };
    const auto dot=[](tetra::Vec3 a,tetra::Vec3 b) {
      return a.x*b.x+a.y*b.y+a.z*b.z;
    };
    return dot(subtract(g[1],g[0]),
               cross(subtract(g[2],g[0]),subtract(g[3],g[0])));
  };
  for(const auto owner:first.logical_owners) {
    CHECK(owner.red_depth()==first.red_depth);
    const auto geometry=tetra::world_tetrahedron_geometry(owner);
    const auto again=tetra::world_tetrahedron_geometry(owner);
    for(std::size_t corner=0U;corner<4U;++corner) {
      CHECK(geometry[corner].x==again[corner].x);
      CHECK(geometry[corner].y==again[corner].y);
      CHECK(geometry[corner].z==again[corner].z);
    }
    CHECK(std::abs(six_volume(geometry))>1.0e-15);
  }

  auto noisy=planar;
  noisy.field=tetra::probes::SandwichField::perlin_height;
  noisy.amplitude=0.22;
  const auto noisy_core=tetra::probes::extract_implicit_bcc_hierarchy_core(noisy);
  CHECK(noisy_core.red_depth==first.red_depth);
  CHECK_FALSE(noisy_core.logical_owners.empty());
  CHECK_FALSE(noisy_core.interface_faces.empty());
  CHECK(noisy_core.interface_face_owners.size()==noisy_core.interface_faces.size());
  CHECK(noisy_core.logical_owners!=first.logical_owners);
}

TEST_CASE("dual contouring is generated on four hexahedra per BCC hierarchy tet") {
  for(const auto [resolution,field,amplitude]:{
      std::tuple{4U,tetra::probes::SandwichField::planar,0.14},
      std::tuple{4U,tetra::probes::SandwichField::perlin_height,0.14},
      std::tuple{6U,tetra::probes::SandwichField::perlin_height,0.14},
      std::tuple{8U,tetra::probes::SandwichField::perlin_height,0.22}}) {
    tetra::probes::SandwichConfig config;
    config.resolution=resolution;
    config.field=field;
    config.amplitude=amplitude;
    const auto first=tetra::probes::extract_bcc_hierarchy_dual_surface(config);
    const auto repeated=tetra::probes::extract_bcc_hierarchy_dual_surface(config);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" vertices="<<first.vertices.size()
         <<" triangles="<<first.triangles.size()
         <<" boundary="<<first.validation.boundary_edges
         <<" nonmanifold="<<first.validation.nonmanifold_edges
         <<" intersections="<<first.validation.strict_triangle_intersections
         <<" rings3="<<first.active_ring_valences[3]
         <<" rings4="<<first.active_ring_valences[4]
         <<" rings6="<<first.active_ring_valences[6]);
    const auto expected_depth=std::bit_width(resolution-1U);
    CHECK(first.red_depth==expected_depth);
    CHECK(first.source_tetrahedra==12U*(std::size_t{1}<<(3U*expected_depth)));
    CHECK(first.source_hexahedra==first.source_tetrahedra*4U);
    CHECK_FALSE(first.vertices.empty());
    CHECK_FALSE(first.triangles.empty());
    CHECK_FALSE(first.active_hexahedron_edges.empty());
    CHECK(first.active_ring_valences[4]>0U);
    CHECK(first.active_ring_valences[3]+first.active_ring_valences[5]+
          first.active_ring_valences[6]+first.active_ring_valences[7]+
          first.active_ring_valences[8]>0U);
    CHECK(first.vertex_owners==repeated.vertex_owners);
    CHECK(first.vertices==repeated.vertices);
    CHECK(first.triangles==repeated.triangles);
    CHECK(first.triangle_owners==repeated.triangle_owners);
    CHECK(first.triangle_owners.size()==first.triangles.size());
    CHECK(first.validation.finite_vertices);
    CHECK(first.validation.nondegenerate_triangles);
    CHECK(first.validation.unique_triangles);
    CHECK(first.validation.manifold_edges);
    CHECK(first.validation.consistently_oriented);
    CHECK(first.validation.no_strict_triangle_intersections);
    CHECK(first.validation.valid);
    for(const auto& owner:first.vertex_owners) {
      CHECK(owner.owner.red_depth()==first.red_depth);
      CHECK(owner.local_hexahedron<4U);
    }
    for(const auto owner:first.triangle_owners)
      CHECK(owner.red_depth()==first.red_depth);
  }
}

TEST_CASE("BCC transition request closes the DC sheet around only its interface halo") {
  for(const auto field:{tetra::probes::SandwichField::planar,
                        tetra::probes::SandwichField::perlin_height}) {
    tetra::probes::SandwichConfig config;
    config.resolution=4U;
    config.field=field;
    config.amplitude=0.14;
    const auto surface=tetra::probes::extract_bcc_hierarchy_dual_surface(config);
    const auto core=tetra::probes::extract_implicit_bcc_hierarchy_core(config);
    const auto request=tetra::probes::make_bcc_surface_core_transition_request(
        surface,core);
    const auto partition=tetra::probes::inspect_bcc_transition_owner_partition(
        surface,core);
    const auto patches=tetra::probes::partition_bcc_transition_parent_stars(
        surface,core);
    const auto cones=tetra::probes::probe_bcc_parent_star_cones(surface,core);
    const auto root_patches=tetra::probes::partition_bcc_transition_stars(
        surface,core,0U);
    const auto root_cones=tetra::probes::probe_bcc_transition_star_cones(
        surface,core,0U);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" failure="<<tetra::probes::bcc_transition_request_failure_name(request.failure)
         <<" input_failure="<<static_cast<unsigned int>(request.validation.failure)
         <<" failing_element="<<request.validation.failing_element
         <<" related_element="<<request.validation.related_element
         <<" owners="<<request.materialized_interface_owners.size()
         <<" far="<<request.implicit_far_core_owners
         <<" faces="<<request.input.outer_faces.size()
         <<" exact_owner_overlap="<<partition.exact_shared_owners
         <<" parent_overlap="<<partition.shared_parent_owners
         <<" parent_face_bounds="<<partition.maximum_surface_faces_per_parent
         <<'/'<<partition.maximum_interface_faces_per_parent
         <<" patches="<<patches.patches.size()
         <<" two_front="<<patches.two_front_patches
         <<" single_loop="<<patches.single_loop_two_front_patches
         <<" shared_seams="<<patches.shared_surface_seams
         <<'/'<<patches.shared_interface_seams
         <<" cones="<<cones.side_walls_accepted
         <<'/'<<cones.closed_boundaries
         <<'/'<<cones.star_shaped_boundaries
         <<" root="<<root_patches.two_front_patches
         <<'/'<<root_patches.single_loop_two_front_patches
         <<'/'<<root_cones.star_shaped_boundaries);
    CHECK(request.accepted());
    CHECK(request.frozen_surface_faces==surface.triangles.size());
    CHECK(request.finite_cap_faces*4U==request.curtain_faces);
    CHECK(request.curtain_faces>0U);
    CHECK(request.hierarchy_interface_faces==core.interface_faces.size());
    CHECK_FALSE(request.materialized_interface_owners.empty());
    CHECK(request.materialized_interface_owners.size()<core.logical_owners.size());
    CHECK(request.implicit_far_core_owners+
              request.materialized_interface_owners.size()==
          core.logical_owners.size());
    CHECK(request.input.retained_core_tetrahedra.size()==
          request.materialized_interface_owners.size());
    CHECK(request.validation.outer_boundary_edges==0U);
    CHECK(request.validation.outer_nonmanifold_edges==0U);
    CHECK(partition.deterministic_bounded_groups);
    CHECK(partition.surface_owners>0U);
    CHECK(partition.interface_owners>0U);
    CHECK(patches.exact_partition);
    CHECK(patches.canonical_shared_seams);
    CHECK(patches.shared_surface_seams>0U);
    CHECK(patches.shared_interface_seams>0U);
    CHECK(patches.two_front_patches==partition.shared_parent_owners);
    CHECK(cones.two_front_patches==patches.two_front_patches);
    CHECK(cones.single_loop_patches==patches.single_loop_two_front_patches);
    CHECK(cones.side_walls_accepted>0U);
    CHECK(cones.closed_boundaries==cones.side_walls_accepted);
    CHECK(root_patches.exact_partition);
    CHECK(root_cones.two_front_patches==root_patches.two_front_patches);
    CHECK(root_cones.closed_patch_boundaries.size()==root_cones.closed_boundaries);
    std::size_t local_seed_facets{},local_seed_facets_recovered{};
    for(const auto& closed:root_cones.closed_patch_boundaries) {
      const auto local=tetra::probes::materialize_canonical_plc_constraints(
          closed.vertices,closed.faces);
      CHECK(local.accepted());
      const auto local_seed=tetra::probes::inspect_canonical_plc_constraints(
          local.constraints,1U<<15U);
      CHECK(local_seed.candidate_tetrahedra>0U);
      local_seed_facets+=local_seed.required_facets;
      local_seed_facets_recovered+=local_seed.recovered_facets;
    }
    INFO("local_seed_facets="<<local_seed_facets_recovered
         <<'/'<<local_seed_facets);
    REQUIRE_FALSE(root_cones.closed_patch_boundaries.empty());
    const auto first_root_constraints=
        tetra::probes::materialize_canonical_plc_constraints(
            root_cones.closed_patch_boundaries.front().vertices,
            root_cones.closed_patch_boundaries.front().faces);
    tetra::probes::CanonicalPlcRecoveryOptions local_options;
    local_options.maximum_vertices=1U<<12U;
    local_options.maximum_facets=1U<<13U;
    local_options.maximum_tetrahedra=1U<<15U;
    local_options.maximum_edge_splits=64U;
    local_options.maximum_advancing_ridge_insertions=64U;
    const auto first_root_recovery=tetra::probes::recover_canonical_plc_edges(
        first_root_constraints.constraints,local_options);
    INFO("first_root_recovery="<<static_cast<unsigned int>(first_root_recovery.failure)
         <<" splits="<<first_root_recovery.edge_splits
         <<" flips="<<first_root_recovery.edge_flips
         <<" facets="<<first_root_recovery.inspection.recovered_facets
         <<'/'<<first_root_recovery.inspection.required_facets);
    CHECK(first_root_recovery.failure!=
          tetra::probes::CanonicalPlcRecoveryFailure::materialization_failed);
    CHECK(first_root_recovery.failure!=
          tetra::probes::CanonicalPlcRecoveryFailure::seed_failed);
    tetra::probes::CanonicalDelaunaySeedInput local_seed_input;
    local_seed_input.maximum_vertices=first_root_constraints.constraints.vertices.size();
    local_seed_input.maximum_tetrahedra=1U<<15U;
    for(const auto& vertex:first_root_constraints.constraints.vertices) {
      local_seed_input.vertices.push_back(vertex.position);
      local_seed_input.stable_vertex_ids.push_back(vertex.id);
    }
    const auto local_background=
        tetra::probes::build_canonical_background_seed(local_seed_input);
    REQUIRE(local_background.accepted());
    const auto local_missing=tetra::probes::inspect_canonical_plc_tetrahedra(
        first_root_constraints.constraints,local_background.tetrahedra);
    REQUIRE_FALSE(local_missing.missing_facets.empty());
    tetra::probes::CanonicalFacetCavityResult direct_facet;
    std::size_t direct_facet_candidates{};
    for(const auto facet:local_missing.missing_facets) {
      ++direct_facet_candidates;
      direct_facet=tetra::probes::recover_literal_facet_by_two_sided_cavity(
          first_root_constraints.constraints,facet,local_background.tetrahedra,
          256U,200000U,512U);
      if(direct_facet.intersected_tetrahedra>0U||direct_facet.accepted)break;
    }
    INFO("direct_facet="<<static_cast<unsigned int>(direct_facet.failure)
         <<" cells="<<direct_facet.intersected_tetrahedra
         <<" trials="<<direct_facet.retriangulation_trials
         <<" candidates="<<direct_facet_candidates);
    CHECK(direct_facet.failure!=tetra::probes::CanonicalFacetCavityFailure::invalid_facet);
    if(field==tetra::probes::SandwichField::planar)
      CHECK(cones.star_shaped_boundaries==cones.single_loop_patches);
    else CHECK(cones.star_shaped_boundaries>0U);
    auto reordered_core=core;
    std::reverse(reordered_core.logical_owners.begin(),reordered_core.logical_owners.end());
    std::reverse(reordered_core.interface_faces.begin(),reordered_core.interface_faces.end());
    std::reverse(reordered_core.interface_face_owners.begin(),
                 reordered_core.interface_face_owners.end());
    const auto reordered_request=
        tetra::probes::make_bcc_surface_core_transition_request(
            surface,reordered_core);
    CHECK(reordered_request.accepted());
    const auto stable_tets=[](const auto& candidate) {
      std::set<std::array<std::uint64_t,4>> result;
      for(const auto& tet:candidate.input.retained_core_tetrahedra) {
        std::array<std::uint64_t,4> stable{};
        for(std::size_t corner=0U;corner<4U;++corner)
          stable[corner]=candidate.input.stable_vertex_ids[tet[corner]];
        std::sort(stable.begin(),stable.end());
        result.insert(stable);
      }
      return result;
    };
    CHECK(stable_tets(request)==stable_tets(reordered_request));
    const auto constraints=tetra::probes::materialize_canonical_plc_constraints(
        request.input);
    CHECK(constraints.accepted());
    CHECK(constraints.constraints.vertices.size()==request.input.vertices.size());
    CHECK(constraints.constraints.facets.size()>request.input.outer_faces.size());
    const auto inspection=tetra::probes::inspect_canonical_plc_constraints(
        constraints.constraints,1U<<18U);
    INFO("seed_failure="<<static_cast<unsigned int>(inspection.failure)
         <<" seed_reason="<<static_cast<unsigned int>(inspection.seed_invalid_reason)
         <<" candidates="<<inspection.candidate_tetrahedra
         <<" recovered="<<inspection.recovered_facets<<'/'<<inspection.required_facets
         <<" edges="<<inspection.recovered_edges<<'/'<<inspection.required_edges);
    CHECK(inspection.failure!=tetra::probes::CanonicalPlcSeedFailure::rejected_manifest);
    CHECK(inspection.candidate_tetrahedra>0U);
    if(field==tetra::probes::SandwichField::planar) {
      tetra::probes::CanonicalPlcRecoveryOptions options;
      options.maximum_vertices=1U<<12U;
      options.maximum_facets=1U<<14U;
      options.maximum_tetrahedra=1U<<18U;
      options.maximum_edge_splits=64U;
      options.maximum_advancing_ridge_insertions=16U;
      options.maximum_intersection_steiner_insertions=4U;
      options.maximum_intersection_steiner_attempts=16U;
      const auto recovery=tetra::probes::recover_canonical_plc_edges(
          constraints.constraints,options);
      INFO("recovery_failure="<<static_cast<unsigned int>(recovery.failure)
           <<" edge_splits="<<recovery.edge_splits
           <<" edge_flips="<<recovery.edge_flips
           <<" recovered_edges="<<recovery.accepted_edge_recoveries
           <<" facets="<<recovery.two_sided_facets_recovered
           <<" missing="<<recovery.inspection.missing_facets.size());
      CHECK(recovery.failure!=tetra::probes::CanonicalPlcRecoveryFailure::materialization_failed);
      CHECK(recovery.failure!=tetra::probes::CanonicalPlcRecoveryFailure::seed_failed);
    }
  }
}
