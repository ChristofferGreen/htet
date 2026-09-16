#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace tetra::probes {

enum class EmbeddingFixture : std::uint8_t { one_tet, two_tet, six_root };

struct EmbeddingValidation {
  // The fixture has an exterior boundary.  A valid open link is permitted
  // there; only complete interior links contribute to the interior claim.
  bool boundary_contract_explicit{};
  bool fixture_oracle_pass{};
  bool optimizer_certified{};
  bool optimizer_controls_pass{};
  bool intersection_controls_pass{};
  bool links_valid{};
  bool no_duplicate_triangles{};
  bool no_degenerate_triangles{};
  bool no_proper_self_intersections{};
  std::size_t shared_vertex_intersection_pairs{};
  std::size_t vertices{};
  std::size_t triangles{};
  std::size_t valence_3_polygons{};
  std::size_t valence_4_polygons{};
  std::size_t valence_6_polygons{};
  std::size_t constrained_qef_fallbacks{};
  std::size_t boundary_rings{};
  std::size_t complete_interior_rings{};
  // Stable JSON fragment describing the first qualified surface failure.
  // It is intentionally data, rather than a prose-only diagnostic, so a
  // later probe can replay the same primal ring and polygon.
  std::string failure_witness;
  bool all_polygon_triangulations_fail{};
  bool non_affine_control_pass{};
  std::size_t zero_sample_edges{};
  double maximum_edge_root_residual{};
  double maximum_kkt_residual{};
  double minimum_double_area{};
  double maximum_triangle_aspect{};
  double mean_vertex_field_error{};
  double maximum_vertex_field_error{};
  std::string failure;
  std::string validator_witness;
};

struct EmbeddingFixtureReport {
  EmbeddingFixture fixture{};
  std::size_t enumerated_fields{};
  std::size_t adversarial_fields{};
  std::size_t valid_surfaces{};
  bool counterexample_found{};
  std::uint64_t counterexample_case{};
  std::array<double, 4> counterexample_coefficients{};
  EmbeddingValidation worst_validation;
};

struct EmbeddingReport {
  std::uint64_t seed{};
  std::size_t budget{};
  std::vector<EmbeddingFixtureReport> fixtures;
  bool strict_dual_survived_search{};
  bool qualification_controls_pass{};
  std::string fixture_identity;
  std::string field_contract;
  std::string boundary_contract;
  std::string non_affine_control_contract;
  std::string conclusion;
};

struct PlacementMethodReport {
  std::string method;
  EmbeddingValidation validation;
  double qef_residual{};
  std::size_t outside_unconstrained_qef{};
  std::vector<std::array<double, 3>> placement_witness;
};

struct PlacementProbeReport {
  std::uint64_t seed{};
  std::size_t search_budget{};
  std::vector<PlacementMethodReport> methods;
  std::size_t feasible_realizations{};
  std::vector<EmbeddingFixtureReport> safe_rule_corpus;
  bool safe_rule_survived_corpus{};
  std::string conclusion;
};

[[nodiscard]] EmbeddingReport run_dual_embedding_probe(std::uint64_t seed, std::size_t budget);
[[nodiscard]] std::string make_dual_embedding_report_json(const EmbeddingReport& report);
[[nodiscard]] std::string embedding_fixture_name(EmbeddingFixture fixture);
[[nodiscard]] PlacementProbeReport run_strict_dual_placement_probe(
    std::uint64_t seed, std::size_t search_budget);
[[nodiscard]] std::string make_strict_dual_placement_report_json(
    const PlacementProbeReport& report);

} // namespace tetra::probes
