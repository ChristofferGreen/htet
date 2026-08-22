#include "tetra_core/implicit_surface.hpp"

#include <algorithm>
#include <cmath>

namespace tetra {
namespace {

double signed_six_volume(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
  const Vec3 ab = b - a, ac = c - a, ad = d - a;
  return ab.x * (ac.y * ad.z - ac.z * ad.y) - ab.y * (ac.x * ad.z - ac.z * ad.x) + ab.z * (ac.x * ad.y - ac.y * ad.x);
}

bool contains_point(const TetMesh& mesh, const Tetrahedron& tet, Vec3 point) {
  const Vec3 a = mesh.vertices().at(tet.vertices[0]), b = mesh.vertices().at(tet.vertices[1]);
  const Vec3 c = mesh.vertices().at(tet.vertices[2]), d = mesh.vertices().at(tet.vertices[3]);
  const double total = signed_six_volume(a, b, c, d);
  if (std::abs(total) < 1e-12) return false;
  const double w0 = signed_six_volume(point, b, c, d) / total;
  const double w1 = signed_six_volume(a, point, c, d) / total;
  const double w2 = signed_six_volume(a, b, point, d) / total;
  const double w3 = signed_six_volume(a, b, c, point) / total;
  constexpr double epsilon = 1e-10;
  return w0 >= -epsilon && w1 >= -epsilon && w2 >= -epsilon && w3 >= -epsilon;
}

double squared_distance_to_segment(Vec3 point, Vec3 first, Vec3 second) {
  const Vec3 edge = second - first;
  const Vec3 offset = point - first;
  const double length_squared = edge.x * edge.x + edge.y * edge.y + edge.z * edge.z;
  const double t = std::clamp((offset.x * edge.x + offset.y * edge.y + offset.z * edge.z) / length_squared, 0.0, 1.0);
  const Vec3 closest{first.x + t * edge.x, first.y + t * edge.y, first.z + t * edge.z};
  const Vec3 delta = point - closest;
  return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
}

double squared_distance_to_triangle(Vec3 point, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 ab = b - a, ac = c - a;
  const Vec3 normal{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
  const double normal_squared = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
  const Vec3 ap = point - a;
  const double signed_plane_distance = normal.x * ap.x + normal.y * ap.y + normal.z * ap.z;
  const Vec3 projected{point.x - normal.x * signed_plane_distance / normal_squared,
                       point.y - normal.y * signed_plane_distance / normal_squared,
                       point.z - normal.z * signed_plane_distance / normal_squared};
  const Vec3 projected_offset = projected - a;
  const double uu = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
  const double uv = ab.x * ac.x + ab.y * ac.y + ab.z * ac.z;
  const double vv = ac.x * ac.x + ac.y * ac.y + ac.z * ac.z;
  const double pu = projected_offset.x * ab.x + projected_offset.y * ab.y + projected_offset.z * ab.z;
  const double pv = projected_offset.x * ac.x + projected_offset.y * ac.y + projected_offset.z * ac.z;
  const double denominator = uu * vv - uv * uv;
  const double u = (vv * pu - uv * pv) / denominator;
  const double v = (uu * pv - uv * pu) / denominator;
  if (u >= 0.0 && v >= 0.0 && u + v <= 1.0) return signed_plane_distance * signed_plane_distance / normal_squared;
  return std::min({squared_distance_to_segment(point, a, b), squared_distance_to_segment(point, b, c), squared_distance_to_segment(point, c, a)});
}

bool sphere_intersects_tetrahedron(const TetMesh& mesh, const Tetrahedron& tet, const Sphere& sphere) {
  if (contains_point(mesh, tet, sphere.centre)) return true;
  constexpr std::array<std::array<std::size_t, 3>, 4> faces{{{{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
  for (const auto& face : faces) {
    const double distance_squared = squared_distance_to_triangle(sphere.centre, mesh.vertices().at(tet.vertices[face[0]]), mesh.vertices().at(tet.vertices[face[1]]), mesh.vertices().at(tet.vertices[face[2]]));
    if (distance_squared <= sphere.radius * sphere.radius) return true;
  }
  return false;
}

}  // namespace

double Sphere::signed_distance(Vec3 point) const {
  const double x = point.x - centre.x;
  const double y = point.y - centre.y;
  const double z = point.z - centre.z;
  return std::sqrt(x * x + y * y + z * z) - radius;
}

SurfaceRelation classify_tetrahedron(const TetMesh& mesh, TetId tet, const Sphere& sphere) {
  const auto& vertices = mesh.tetrahedron(tet).vertices;
  bool has_negative = false;
  bool has_positive = false;
  for (const VertexId vertex : vertices) {
    const double distance = sphere.signed_distance(mesh.vertices().at(vertex));
    has_negative |= distance < 0.0;
    has_positive |= distance > 0.0;
  }
  // A sphere is convex, so a tetrahedron whose four vertices are inside is
  // wholly inside as well.  Do this before the conservative overlap test:
  // the latter also detects the sphere centre inside a tet and would wrongly
  // turn an interior cell back into a boundary cell.
  if (!has_positive) return SurfaceRelation::inside;
  if (has_negative && has_positive) return SurfaceRelation::intersecting;
  // A same-sign test misses both a sphere contained by a tetrahedron and a
  // sphere that merely clips one of its faces.  Use the closest point on the
  // tetrahedron boundary for the conservative sphere-versus-tetrahedron test.
  if (sphere_intersects_tetrahedron(mesh, mesh.tetrahedron(tet), sphere)) return SurfaceRelation::intersecting;
  return SurfaceRelation::outside;
}

double projected_tetrahedron_diameter(const TetMesh& mesh, TetId tet, const Camera& camera) {
  const auto& vertices = mesh.tetrahedron(tet).vertices;
  Vec3 centre{};
  for (const VertexId vertex : vertices) centre = centre + mesh.vertices().at(vertex);
  centre = centre / 4.0;
  double radius_squared = 0.0;
  for (const VertexId vertex : vertices) {
    const Vec3 delta = mesh.vertices().at(vertex) - centre;
    radius_squared = std::max(radius_squared, delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  }
  const Vec3 view = centre - camera.position;
  const double distance = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
  if (distance <= std::sqrt(radius_squared)) return camera.viewport_height_pixels;
  const double focal_length = (camera.viewport_height_pixels * 0.5) / std::tan(camera.vertical_fov_radians * 0.5);
  return (2.0 * std::sqrt(radius_squared) * focal_length) / distance;
}

std::vector<TetId> mark_oversized_intersections(const TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold) {
  std::vector<TetId> marked;
  for (const TetId id : mesh.active_leaves()) {
    if (classify_tetrahedron(mesh, id, sphere) == SurfaceRelation::intersecting &&
        projected_tetrahedron_diameter(mesh, id, camera) > pixel_threshold) marked.push_back(id);
  }
  return marked;
}

AdaptiveResult refine_to_sphere(TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold, unsigned int maximum_depth) {
  AdaptiveResult result;
  const unsigned int increment=subdivision_depth_increment(mesh.subdivision_method());
  while(true){
    const auto oversized=mark_oversized_intersections(mesh,sphere,camera,pixel_threshold);
    std::vector<TetId> marked;
    marked.reserve(oversized.size());
    for(const auto id:oversized)if(mesh.refinement_depth(id)+increment<=maximum_depth)marked.push_back(id);
    if(marked.empty()){
      result.reached_depth_limit=!oversized.empty();
      return result;
    }
    result.refined_leaves += marked.size();
    ++result.iterations;
    mesh.refine_selected_binary(marked);
  }
}

std::vector<Triangle> extract_isosurface(const TetMesh& mesh, const Sphere& sphere) {
  std::vector<Triangle> triangles;
  constexpr std::array<std::array<int, 2>, 6> edges{{{{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};
  const auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
  const auto dot = [](Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  for (const TetId id : mesh.active_leaves()) {
    const auto& tet = mesh.tetrahedron(id).vertices;
    std::array<Vec3, 4> points{};
    std::array<double, 4> distances{};
    for (std::size_t i = 0; i < points.size(); ++i) {
      points[i] = mesh.vertices().at(tet[i]);
      distances[i] = sphere.signed_distance(points[i]);
    }
    std::vector<Vec3> crossings;
    for (const auto& edge : edges) {
      const std::size_t first = static_cast<std::size_t>(edge[0]), second = static_cast<std::size_t>(edge[1]);
      if ((distances[first] < 0.0) == (distances[second] < 0.0)) continue;
      // The field is an analytic sphere: solve the edge/sphere intersection
      // exactly instead of interpolating signed distances linearly.
      const Vec3 offset = points[first] - sphere.centre;
      const Vec3 direction = points[second] - points[first];
      const double qa = dot(direction, direction);
      const double qb = 2.0 * dot(offset, direction);
      const double qc = dot(offset, offset) - sphere.radius * sphere.radius;
      const double discriminant = std::max(0.0, qb * qb - 4.0 * qa * qc);
      const double root0 = (-qb - std::sqrt(discriminant)) / (2.0 * qa);
      const double root1 = (-qb + std::sqrt(discriminant)) / (2.0 * qa);
      const double fraction = root0 >= 0.0 && root0 <= 1.0 ? root0 : root1;
      crossings.push_back({points[first].x + fraction * (points[second].x - points[first].x),
                           points[first].y + fraction * (points[second].y - points[first].y),
                           points[first].z + fraction * (points[second].z - points[first].z)});
    }
    if (crossings.size() < 3) continue;
    Vec3 centre{};
    for (const Vec3 point : crossings) centre = centre + point;
    centre = centre / static_cast<double>(crossings.size());
    Vec3 normal = centre - sphere.centre;
    const double normal_length = std::sqrt(dot(normal, normal));
    if (normal_length < 1e-12) continue;
    normal = normal / normal_length;
    const Vec3 reference = std::abs(normal.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0};
    const Vec3 axis_u = cross(reference, normal);
    const Vec3 axis_v = cross(normal, axis_u);
    std::sort(crossings.begin(), crossings.end(), [&](Vec3 left, Vec3 right) {
      const Vec3 left_offset = left - centre, right_offset = right - centre;
      return std::atan2(dot(left_offset, axis_v), dot(left_offset, axis_u)) < std::atan2(dot(right_offset, axis_v), dot(right_offset, axis_u));
    });
    for (std::size_t index = 1; index + 1 < crossings.size(); ++index) triangles.push_back({crossings[0], crossings[index], crossings[index + 1]});
  }
  return triangles;

  constexpr std::array<std::array<int, 2>, 6> legacy_edges{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  const auto sphere_intersects_simplex = [&sphere](const std::array<Vec3, 4>& vertices) {
    const auto signed_volume = [](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
      return signed_six_volume(a, b, c, d);
    };
    const double total = signed_volume(vertices[0], vertices[1], vertices[2], vertices[3]);
    if (std::abs(total) > 1e-12) {
      const double w0 = signed_volume(sphere.centre, vertices[1], vertices[2], vertices[3]) / total;
      const double w1 = signed_volume(vertices[0], sphere.centre, vertices[2], vertices[3]) / total;
      const double w2 = signed_volume(vertices[0], vertices[1], sphere.centre, vertices[3]) / total;
      const double w3 = signed_volume(vertices[0], vertices[1], vertices[2], sphere.centre) / total;
      constexpr double epsilon = 1e-10;
      if (w0 >= -epsilon && w1 >= -epsilon && w2 >= -epsilon && w3 >= -epsilon) return true;
    }
    constexpr std::array<std::array<std::size_t, 3>, 4> faces{{{{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
    for (const auto& face : faces) {
      if (squared_distance_to_triangle(sphere.centre, vertices[face[0]], vertices[face[1]], vertices[face[2]]) <= sphere.radius * sphere.radius) return true;
    }
    return false;
  };
  const auto polygonize = [&sphere, &triangles, &legacy_edges, &sphere_intersects_simplex](auto&& self, const std::array<Vec3, 4>& vertices, unsigned int remaining_depth) -> void {
    std::vector<Vec3> crossings;
    std::array<double, 4> distances{};
    bool has_negative = false;
    bool has_positive = false;
    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
      distances[vertex] = sphere.signed_distance(vertices[vertex]);
      has_negative |= distances[vertex] < 0.0;
      has_positive |= distances[vertex] > 0.0;
    }
    if (!has_negative || !has_positive) {
      // If all four corners are inside, convexity guarantees that this whole
      // simplex is inside the sphere.  For an all-outside simplex, however,
      // the sphere can pass through a face interior without touching any edge.
      // Subdivide that case before declaring it empty.
      if (has_positive && remaining_depth > 0 && sphere_intersects_simplex(vertices)) {
        const Vec3 ab = (vertices[0] + vertices[1]) / 2.0, ac = (vertices[0] + vertices[2]) / 2.0;
        const Vec3 ad = (vertices[0] + vertices[3]) / 2.0, bc = (vertices[1] + vertices[2]) / 2.0;
        const Vec3 bd = (vertices[1] + vertices[3]) / 2.0, cd = (vertices[2] + vertices[3]) / 2.0;
        const std::array<std::array<Vec3, 4>, 8> children{{
            {{vertices[0], ab, ac, ad}}, {{ab, vertices[1], bc, bd}},
            {{ac, bc, vertices[2], cd}}, {{ad, bd, cd, vertices[3]}},
            {{ab, ac, ad, cd}}, {{ab, ac, bc, cd}}, {{ab, ad, bd, cd}}, {{ab, bc, bd, cd}},
        }};
        for (const auto& child : children) self(self, child, remaining_depth - 1);
      }
      return;
    }
    for (const auto& edge : legacy_edges) {
      const auto first = static_cast<std::size_t>(edge[0]);
      const auto second = static_cast<std::size_t>(edge[1]);
      const Vec3 a = vertices[first], b = vertices[second];
      const double da = distances[first], db = distances[second];
      if ((da < 0.0) == (db < 0.0)) continue;
      // The experiment's field is an analytic sphere, so intersect its
      // quadratic exactly instead of treating the endpoint SDF values as a
      // linear field.  This keeps the displayed/extracted surface on the
      // declared sphere even in coarse leaves.
      const Vec3 offset{a.x - sphere.centre.x, a.y - sphere.centre.y, a.z - sphere.centre.z};
      const Vec3 direction{b.x - a.x, b.y - a.y, b.z - a.z};
      const double quadratic_a = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
      const double quadratic_b = 2.0 * (offset.x * direction.x + offset.y * direction.y + offset.z * direction.z);
      const double quadratic_c = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z - sphere.radius * sphere.radius;
      const double discriminant = std::max(0.0, quadratic_b * quadratic_b - 4.0 * quadratic_a * quadratic_c);
      const double root0 = (-quadratic_b - std::sqrt(discriminant)) / (2.0 * quadratic_a);
      const double root1 = (-quadratic_b + std::sqrt(discriminant)) / (2.0 * quadratic_a);
      const double t = root0 >= 0.0 && root0 <= 1.0 ? root0 : root1;
      const Vec3 crossing{a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t};
      const auto same_point = [&crossing](const Vec3& existing) {
        const Vec3 delta = existing - crossing;
        return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z < 1e-20;
      };
      if (std::ranges::none_of(crossings, same_point)) crossings.push_back(crossing);
    }
    if (crossings.size() == 3) triangles.push_back({crossings[0], crossings[1], crossings[2]});
    if (crossings.size() == 4) {
      triangles.push_back({crossings[0], crossings[1], crossings[2]});
      triangles.push_back({crossings[0], crossings[2], crossings[3]});
    }
  };
  for (const TetId id : mesh.active_leaves()) {
    const auto& tet = mesh.tetrahedron(id);
    const std::array<Vec3, 4> vertices{{mesh.vertices().at(tet.vertices[0]), mesh.vertices().at(tet.vertices[1]), mesh.vertices().at(tet.vertices[2]), mesh.vertices().at(tet.vertices[3])}};
    polygonize(polygonize, vertices, 3);
  }
  return triangles;
}

std::vector<Triangle> extract_dual_contour(const TetMesh& mesh, const Sphere& sphere) {
  constexpr std::array<std::array<std::size_t, 2>, 6> tet_edges{{
      {{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};
  const auto dot = [](Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  const auto cross = [](Vec3 a, Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
  };
  const auto length_squared = [&dot](Vec3 value) { return dot(value, value); };
  const auto normalize = [&length_squared](Vec3 value) {
    const double length = std::sqrt(length_squared(value));
    return length > 1e-15 ? value / length : Vec3{};
  };
  const auto packed_edge = [](VertexId first, VertexId second) {
    if (second < first) std::swap(first, second);
    return (static_cast<std::uint64_t>(first) << 32U) | static_cast<std::uint64_t>(second);
  };
  const auto edge_crossing = [&sphere, &dot](Vec3 first, Vec3 second) {
    const Vec3 offset = first-sphere.centre;
    const Vec3 direction = second-first;
    const double a = dot(direction, direction);
    const double b = 2.0*dot(offset, direction);
    const double c = dot(offset, offset)-sphere.radius*sphere.radius;
    const double discriminant = std::max(0.0, b*b-4.0*a*c);
    const double root0 = (-b-std::sqrt(discriminant))/(2.0*a);
    const double root1 = (-b+std::sqrt(discriminant))/(2.0*a);
    const double t = root0 >= 0.0 && root0 <= 1.0 ? root0 : root1;
    return Vec3{first.x+t*direction.x, first.y+t*direction.y, first.z+t*direction.z};
  };

  struct EdgeIncident {
    std::uint64_t edge{};
    std::uint32_t cell{};
  };
  std::vector<Vec3> dual_vertices;
  std::vector<EdgeIncident> incidents;
  dual_vertices.reserve(mesh.active_leaves().size()/4);
  incidents.reserve(mesh.active_leaves().size());

  for (const TetId id : mesh.active_leaves()) {
    const auto& tet = mesh.tetrahedron(id);
    std::array<Vec3, 4> points{};
    std::array<double, 4> distances{};
    for (std::size_t index = 0; index < 4; ++index) {
      points[index] = mesh.vertices()[tet.vertices[index]];
      distances[index] = sphere.signed_distance(points[index]);
    }

    std::array<Vec3, 6> crossings{};
    std::array<Vec3, 6> normals{};
    std::size_t crossing_count = 0;
    std::array<std::uint64_t, 6> crossed_edges{};
    std::size_t crossed_edge_count = 0;
    for (const auto edge : tet_edges) {
      const auto first = edge[0], second = edge[1];
      if ((distances[first] < 0.0) == (distances[second] < 0.0)) continue;
      const Vec3 crossing = edge_crossing(points[first], points[second]);
      crossings[crossing_count] = crossing;
      normals[crossing_count] = normalize(crossing-sphere.centre);
      ++crossing_count;
      crossed_edges[crossed_edge_count++] = packed_edge(tet.vertices[first], tet.vertices[second]);
    }
    if (crossing_count < 3) continue;

    Vec3 mass_point{};
    for (std::size_t sample = 0; sample < crossing_count; ++sample)
      mass_point = mass_point+crossings[sample];
    mass_point = mass_point/static_cast<double>(crossing_count);

    // Minimize the Hermite plane error. A small pull toward the mass point
    // regularizes the nearly parallel normals found in small smooth cells.
    double augmented[3][4]{};
    for (std::size_t sample = 0; sample < crossing_count; ++sample) {
      const double n[3]{normals[sample].x, normals[sample].y, normals[sample].z};
      const double plane = dot(normals[sample], crossings[sample]);
      for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column)
          augmented[row][column] += n[row]*n[column];
        augmented[row][3] += n[row]*plane;
      }
    }
    const double regularization = 1e-2*static_cast<double>(crossing_count);
    const double mass[3]{mass_point.x, mass_point.y, mass_point.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      augmented[axis][axis] += regularization;
      augmented[axis][3] += regularization*mass[axis];
    }
    bool solvable = true;
    for (std::size_t column = 0; column < 3; ++column) {
      std::size_t pivot = column;
      for (std::size_t row = column+1; row < 3; ++row)
        if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
      if (std::abs(augmented[pivot][column]) < 1e-12) {
        solvable = false;
        break;
      }
      if (pivot != column) for (std::size_t entry = column; entry < 4; ++entry)
        std::swap(augmented[pivot][entry], augmented[column][entry]);
      const double divisor = augmented[column][column];
      for (std::size_t entry = column; entry < 4; ++entry) augmented[column][entry] /= divisor;
      for (std::size_t row = 0; row < 3; ++row) {
        if (row == column) continue;
        const double factor = augmented[row][column];
        for (std::size_t entry = column; entry < 4; ++entry)
          augmented[row][entry] -= factor*augmented[column][entry];
      }
    }
    Vec3 dual = solvable ? Vec3{augmented[0][3], augmented[1][3], augmented[2][3]} : mass_point;
    if (!contains_point(mesh, tet, dual)) {
      // The cell owns its dual vertex. Constrain an outlying QEF solution by
      // walking back toward the crossing mass point, which is inside the
      // convex tetrahedron.
      double lower = 0.0, upper = 1.0;
      for (unsigned int iteration = 0; iteration < 48; ++iteration) {
        const double t = (lower+upper)*0.5;
        const Vec3 candidate{mass_point.x+t*(dual.x-mass_point.x),
                             mass_point.y+t*(dual.y-mass_point.y),
                             mass_point.z+t*(dual.z-mass_point.z)};
        if (contains_point(mesh, tet, candidate)) lower = t;
        else upper = t;
      }
      dual = {mass_point.x+lower*(dual.x-mass_point.x),
              mass_point.y+lower*(dual.y-mass_point.y),
              mass_point.z+lower*(dual.z-mass_point.z)};
    }
    Vec3 tet_centre{};
    for (const Vec3 point : points) tet_centre=tet_centre+point;
    tet_centre=tet_centre/4.0;
    // Dual topology assigns a distinct vertex to every cut cell. Keep it
    // strictly inside its owner so adjacent constrained QEF solutions cannot
    // collapse onto the same face and create zero-area dual polygons.
    constexpr double ownership_inset = 1e-4;
    dual={dual.x*(1.0-ownership_inset)+tet_centre.x*ownership_inset,
          dual.y*(1.0-ownership_inset)+tet_centre.y*ownership_inset,
          dual.z*(1.0-ownership_inset)+tet_centre.z*ownership_inset};

    const auto cell = static_cast<std::uint32_t>(dual_vertices.size());
    dual_vertices.push_back(dual);
    for (std::size_t edge = 0; edge < crossed_edge_count; ++edge)
      incidents.push_back({crossed_edges[edge], cell});
  }

  std::sort(incidents.begin(), incidents.end(), [](const EdgeIncident& first, const EdgeIncident& second) {
    return first.edge < second.edge || (first.edge == second.edge && first.cell < second.cell);
  });
  std::vector<Triangle> triangles;
  triangles.reserve(incidents.size()*2);
  std::vector<Vec3> polygon;
  polygon.reserve(16);
  for (std::size_t begin = 0; begin < incidents.size();) {
    std::size_t end = begin+1;
    while (end < incidents.size() && incidents[end].edge == incidents[begin].edge) ++end;
    if (end-begin < 3) {
      begin = end;
      continue;
    }

    const VertexId first_id = static_cast<VertexId>(incidents[begin].edge >> 32U);
    const VertexId second_id = static_cast<VertexId>(incidents[begin].edge);
    const Vec3 first = mesh.vertices()[first_id], second = mesh.vertices()[second_id];
    const Vec3 edge_point = edge_crossing(first, second);
    const Vec3 axis = normalize(second-first);
    const Vec3 reference = std::abs(axis.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0};
    const Vec3 basis_u = normalize(cross(reference, axis));
    const Vec3 basis_v = cross(axis, basis_u);
    polygon.clear();
    polygon.reserve(end-begin);
    for (std::size_t incident = begin; incident < end; ++incident)
      polygon.push_back(dual_vertices[incidents[incident].cell]);
    std::sort(polygon.begin(), polygon.end(), [&](Vec3 left, Vec3 right) {
      const Vec3 left_offset = left-edge_point, right_offset = right-edge_point;
      return std::atan2(dot(left_offset, basis_v), dot(left_offset, basis_u)) <
             std::atan2(dot(right_offset, basis_v), dot(right_offset, basis_u));
    });

    const Vec3 polygon_normal = cross(polygon[1]-polygon[0], polygon[2]-polygon[0]);
    if (dot(polygon_normal, edge_point-sphere.centre) < 0.0)
      std::reverse(polygon.begin(), polygon.end());
    // A ring of constrained QEF vertices is not necessarily convex. A fan
    // from one ring vertex can cross outside the dual face and overlap other
    // faces. The exact field crossing on the primal edge lies in every local
    // edge wedge, so triangulating consecutive ring vertices through it keeps
    // every triangle local and exposes the complete surface wireframe.
    for (std::size_t index = 0; index < polygon.size(); ++index) {
      Triangle triangle{edge_point,polygon[index],polygon[(index+1)%polygon.size()]};
      const Vec3 triangle_normal=cross(triangle.b-triangle.a,triangle.c-triangle.a);
      const Vec3 triangle_centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                                 (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                                 (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
      if (dot(triangle_normal,triangle_centre-sphere.centre) < 0.0)
        std::swap(triangle.b,triangle.c);
      if (length_squared(triangle_normal) > 1e-24) triangles.push_back(triangle);
    }
    begin = end;
  }
  return triangles;
}

}  // namespace tetra
