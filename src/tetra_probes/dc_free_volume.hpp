#pragma once

#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/sandwich_probe.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tetra::probes {

// A closed, literal DC boundary with no retained regular-grid core.  Vertex
// IDs are stable only within the frozen surface revision; generated interior
// IDs belong to the tetrahedralizer result.
struct DcFreeVolumeInput {
  std::vector<FrozenFacetVertex> vertices;
  std::vector<std::array<std::uint64_t,3>> faces;
};

enum class DcFreeVolumeFailure : std::uint8_t {
  none,
  invalid_frozen_surface,
  tetrahedralization_failed,
};

struct DcFreeVolumeResult {
  DcFreeVolumeFailure failure{DcFreeVolumeFailure::invalid_frozen_surface};
  DcFreeVolumeInput input;
  ClosedPlcTetrahedralizationResult volume;

  [[nodiscard]] bool accepted() const noexcept {
    return failure==DcFreeVolumeFailure::none&&volume.accepted();
  }
};

// Adapts the contained closed DC surface itself, never the fixture's outer
// closure or retained core. The source triangles remain literal constraints.
[[nodiscard]] DcFreeVolumeInput make_dc_free_volume_input(
    const AdvancingFrontFixture& fixture);

[[nodiscard]] DcFreeVolumeResult construct_dc_free_volume(
    const AdvancingFrontFixture& fixture,
    const ClosedPlcTetrahedralizationOptions& options={});

} // namespace tetra::probes
