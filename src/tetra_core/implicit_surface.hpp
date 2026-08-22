#pragma once

#include "tetra_core/tet_mesh.hpp"

namespace tetra {

enum class ImplicitShapeKind : std::uint8_t {
  sphere,
  merging_spheres,
  cube,
  capped_cylinder,
  perlin_terrain,
  torus,
  cone,
  gyroid,
  rounded_cube,
};

inline constexpr std::array<ImplicitShapeKind,9> implicit_shape_kinds{
    ImplicitShapeKind::sphere,ImplicitShapeKind::merging_spheres,
    ImplicitShapeKind::cube,ImplicitShapeKind::capped_cylinder,
    ImplicitShapeKind::perlin_terrain,ImplicitShapeKind::torus,
    ImplicitShapeKind::cone,ImplicitShapeKind::gyroid,
    ImplicitShapeKind::rounded_cube};

[[nodiscard]] std::string_view implicit_shape_name(ImplicitShapeKind kind);
[[nodiscard]] std::string_view implicit_shape_key(ImplicitShapeKind kind);
[[nodiscard]] double implicit_shape_default_secondary(ImplicitShapeKind kind);

struct Sphere {
  Vec3 centre{0.5, 0.5, 0.5};
  double radius{0.35};
  ImplicitShapeKind kind{ImplicitShapeKind::sphere};
  double secondary{0.12};
  double frequency{3.0};

  [[nodiscard]] double signed_distance(Vec3 point) const;
  [[nodiscard]] Vec3 normal(Vec3 point) const;
  [[nodiscard]] Vec3 edge_intersection(Vec3 first,Vec3 second) const;
  [[nodiscard]] Vec3 project_to_surface(Vec3 point) const;
};

struct Camera {
  Vec3 position{0.5, 0.5, 3.0};
  double vertical_fov_radians{0.7853981633974483};
  double viewport_height_pixels{800.0};
  Vec3 forward{0.0,0.0,-1.0};
  Vec3 up{0.0,1.0,0.0};
  double aspect_ratio{1.0};
};

[[nodiscard]] double projected_tetrahedron_diameter(const TetMesh& mesh, TetId tet, const Camera& camera);
[[nodiscard]] std::vector<TetId> mark_oversized_intersections(const TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold);

struct AdaptiveResult { std::size_t iterations{}; std::size_t refined_leaves{}; bool reached_depth_limit{}; };

// Camera motion changes projected size, but not the implicit field sampled at
// persistent mesh vertices.  A viewer can retain this cache across camera
// reconciliations and clear it when replacing the mesh.
struct ImplicitValueCache {
  std::vector<double> vertex_distances;
  Sphere sampled_surface{};
  bool has_sampled_surface{};
  void clear() noexcept {
    vertex_distances.clear();
    has_sampled_surface=false;
  }
};

[[nodiscard]] AdaptiveResult refine_to_sphere(
    TetMesh& mesh,const Sphere& sphere,const Camera& camera,double pixel_threshold,
    unsigned int maximum_depth,ImplicitValueCache* value_cache=nullptr);

struct Triangle { Vec3 a; Vec3 b; Vec3 c; };
[[nodiscard]] std::vector<Triangle> extract_isosurface(const TetMesh& mesh, const Sphere& sphere);
// Tetrahedral dual contouring: one constrained QEF vertex per sign-changing
// active leaf, connected into polygons around sign-changing primal edges.
[[nodiscard]] std::vector<Triangle> extract_dual_contour(const TetMesh& mesh, const Sphere& sphere);

enum class SurfaceRelation { inside, outside, intersecting };

// Conservative classification: a tetrahedron may be reported as intersecting
// when the field cannot prove a uniform sign, but an actual implicit-surface
// intersection is never reported as inside or outside.
[[nodiscard]] SurfaceRelation classify_tetrahedron(const TetMesh& mesh, TetId tet, const Sphere& sphere);

}  // namespace tetra
