#pragma once

#include "tetra_core/tet_mesh.hpp"

namespace tetra::probes {

// Exact signs over the binary values already supplied by the caller.  These
// predicates neither round coordinates nor perturb the constrained geometry;
// internally they evaluate determinants of dyadic rationals with a compact
// in-project arbitrary-precision integer.
enum class ExactPredicateSign : int { negative=-1, zero=0, positive=1 };

[[nodiscard]] ExactPredicateSign exact_orientation_3d(
    Vec3 a, Vec3 b, Vec3 c, Vec3 d);
// Correctly-rounded binary64 value of the exact determinant of the supplied
// binary64 coordinates. This is for algorithms, such as Wang's placement
// routine, whose source consumes an orientation magnitude as data rather
// than only its sign.
[[nodiscard]] double exact_orientation_3d_value(
    Vec3 a, Vec3 b, Vec3 c, Vec3 d);

// The sign of the translated in-sphere determinant for a,b,c,d against
// query. For a positively oriented (a,b,c,d), an inside query is negative;
// reversing the tetrahedron orientation reverses that interpretation.
[[nodiscard]] ExactPredicateSign exact_in_sphere(
    Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 query);

} // namespace tetra::probes
