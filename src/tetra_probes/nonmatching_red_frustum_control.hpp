#pragma once

#include "tetra_probes/surface_core_contract.hpp"

namespace tetra::probes {
// Bounded geometry control: a convex homothetic coarse tetrahedron pair.
// It refines only outer edges; no N6, TetGen, or arbitrary PLC path exists.
struct NonmatchingRedFrustumInput { std::array<Vec3,4> inner{}, outer{}; double scale{1.0}; };
enum class NonmatchingRedFrustumFailure : std::uint8_t { none, off_contract, geometry_rejected, quality_rejected };
struct NonmatchingRedFrustumResult { NonmatchingRedFrustumFailure failure{NonmatchingRedFrustumFailure::off_contract}; SurfaceCoreTransitionOutput output; SurfaceCoreTransitionValidation validation; double minimum_dihedral{},maximum_dihedral{}; [[nodiscard]] bool accepted()const{return failure==NonmatchingRedFrustumFailure::none;} };
[[nodiscard]] NonmatchingRedFrustumResult construct_nonmatching_red_frustum_control(const NonmatchingRedFrustumInput& input);
}
