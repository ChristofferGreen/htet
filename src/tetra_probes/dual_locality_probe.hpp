#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tetra::probes {

enum class LocalityField : std::uint8_t { plane, sphere };

[[nodiscard]] std::string_view locality_field_name(LocalityField field);

struct LocalityScaleResult {
  unsigned int domain_radius{};
  std::size_t hexahedra{};
  std::size_t crossed_edges{};
  std::size_t seed_cells{};
  std::size_t closure_cells{};
  std::size_t closure_crossed_edges{};
  std::size_t closure_valence_3{};
  std::size_t closure_valence_4{};
  std::size_t closure_valence_6{};
  bool closure_touches_outer_boundary{};
  std::uint64_t closure_hash{};
};

enum class LocalityClassification : std::uint8_t { bounded, percolating };

struct LocalityFieldReport {
  LocalityField field{};
  std::vector<LocalityScaleResult> scales;
  LocalityClassification classification{};
  std::string reason;
};

// Builds a frozen repetition of the six-Freudenthal-tetrahedron root complex.
// The request remains the fixed cube [-0.5, 0.5]^3 while the root-cube domain
// expands from radius 1 through max_domain_radius.  A selected cell owns every
// sign-changing primal edge for which it is the canonical lowest-id owner;
// closure repeatedly includes each complete incident-cell support ring.
[[nodiscard]] LocalityFieldReport run_dual_locality_probe(
    LocalityField field, unsigned int max_domain_radius);

[[nodiscard]] std::string make_dual_locality_report_json(
    const std::vector<LocalityFieldReport>& reports);

} // namespace tetra::probes
