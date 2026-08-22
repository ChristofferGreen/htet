#pragma once

#include "tetra_core/tet_mesh.hpp"

namespace tetra {

struct Sphere {
  Vec3 centre{0.5, 0.5, 0.5};
  double radius{0.35};

  [[nodiscard]] double signed_distance(Vec3 point) const;
};

struct Camera {
  Vec3 position{0.5, 0.5, 3.0};
  double vertical_fov_radians{0.7853981633974483};
  double viewport_height_pixels{800.0};
};

[[nodiscard]] double projected_tetrahedron_diameter(const TetMesh& mesh, TetId tet, const Camera& camera);
[[nodiscard]] std::vector<TetId> mark_oversized_intersections(const TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold);

struct AdaptiveResult { std::size_t iterations{}; std::size_t refined_leaves{}; bool reached_depth_limit{}; };
[[nodiscard]] AdaptiveResult refine_to_sphere(TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold, unsigned int maximum_depth);

struct Triangle { Vec3 a; Vec3 b; Vec3 c; };
[[nodiscard]] std::vector<Triangle> extract_isosurface(const TetMesh& mesh, const Sphere& sphere);
// Tetrahedral dual contouring: one constrained QEF vertex per sign-changing
// active leaf, connected into polygons around sign-changing primal edges.
[[nodiscard]] std::vector<Triangle> extract_dual_contour(const TetMesh& mesh, const Sphere& sphere);

enum class SurfaceRelation { inside, outside, intersecting };

// Conservative classification: a tetrahedron may be reported as intersecting
// even when it merely contains the sphere, but an actual intersection is never
// reported as inside or outside.
[[nodiscard]] SurfaceRelation classify_tetrahedron(const TetMesh& mesh, TetId tet, const Sphere& sphere);

}  // namespace tetra
