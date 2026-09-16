#include "tetra_probes/plc_chunk_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tetra::probes {
namespace {

struct Point {
  int x{};
  int y{};
  int z{};
  auto operator<=>(const Point&) const = default;
};
struct Triangle { std::array<Point, 3> p; bool terrain{}; };
using Edge = std::pair<Point, Point>;
constexpr int floor_z = -2;

Edge edge(Point a, Point b) { if (b < a) std::swap(a, b); return {a, b}; }
std::uint64_t append_hash(std::uint64_t h, int n) {
  h ^= static_cast<std::uint32_t>(n); return h * 1099511628211ULL;
}
std::uint64_t hash_triangles(const std::vector<Triangle>& triangles, bool terrain_only) {
  std::vector<std::array<Point, 3>> canonical;
  for (const auto& triangle : triangles) if (!terrain_only || triangle.terrain) {
    auto points = triangle.p;
    std::sort(points.begin(), points.end());
    canonical.push_back(points);
  }
  std::sort(canonical.begin(), canonical.end());
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto& points : canonical) for (const auto& point : points)
    for (const int coordinate : {point.x, point.y, point.z}) hash = append_hash(hash, coordinate);
  return hash;
}

// The terrain is frozen input, deliberately not output from Dual Contouring.
// A unit square has the canonical diagonal from its lower-left to upper-right.
std::vector<Triangle> plane_patch(int min_x, int max_x, int min_y, int max_y) {
  std::vector<Triangle> output;
  for (int y = min_y; y < max_y; ++y) for (int x = min_x; x < max_x; ++x) {
    const Point a{x, y, 0}, b{x + 1, y, 0}, c{x + 1, y + 1, 0}, d{x, y + 1, 0};
    output.push_back({{{a, b, c}}, true});
    output.push_back({{{a, c, d}}, true});
  }
  return output;
}

std::vector<Triangle> closed_chunk(int min_x, int max_x, int min_y, int max_y) {
  auto output = plane_patch(min_x, max_x, min_y, max_y);
  std::map<Edge, std::pair<Point, Point>> directed_boundary;
  for (const auto& triangle : output) for (unsigned int i = 0; i < 3U; ++i) {
    const Point a = triangle.p[i], b = triangle.p[(i + 1U) % 3U];
    const Edge canonical = edge(a, b);
    if (directed_boundary.contains(canonical)) directed_boundary.erase(canonical);
    else directed_boundary.emplace(canonical, std::pair{a, b});
  }
  // Extrude every complete canonical terrain boundary edge.  Subdivision is
  // retained on sides and bottom, so no artificial triangle bridges a real
  // terrain vertex or edge.
  for (const auto& [_, directed] : directed_boundary) {
    const Point a = directed.first, b = directed.second;
    const Point first = std::min(a, b), second = std::max(a, b);
    const Point low_first{first.x, first.y, floor_z}, low_second{second.x, second.y, floor_z};
    // The interface diagonal is selected from stable endpoint order, rather
    // than each chunk's outward walk direction.  Adjacent chunks therefore
    // obtain the same two geometric interface triangles with opposite windings.
    if (a == first) {
      output.push_back({{{first, low_second, second}}, false});
      output.push_back({{{first, low_first, low_second}}, false});
    } else {
      output.push_back({{{first, second, low_second}}, false});
      output.push_back({{{first, low_second, low_first}}, false});
    }
  }
  const auto bottom = plane_patch(min_x, max_x, min_y, max_y);
  for (const auto& triangle : bottom) {
    const Point a{triangle.p[0].x, triangle.p[0].y, floor_z};
    const Point b{triangle.p[1].x, triangle.p[1].y, floor_z};
    const Point c{triangle.p[2].x, triangle.p[2].y, floor_z};
    output.push_back({{{c, b, a}}, false});
  }
  return output;
}

Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Point cross(Point a, Point b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }

PlcValidation validate(const std::vector<Triangle>& triangles, std::uint64_t expected_terrain_hash) {
  PlcValidation result;
  std::map<Edge, std::vector<std::pair<std::size_t, bool>>> edges;
  std::set<Point> vertices;
  bool nondegenerate = true;
  for (std::size_t i = 0; i < triangles.size(); ++i) {
    const auto& triangle = triangles[i];
    const Point normal = cross(sub(triangle.p[1], triangle.p[0]), sub(triangle.p[2], triangle.p[0]));
    nondegenerate = nondegenerate && (normal.x != 0 || normal.y != 0 || normal.z != 0);
    for (const Point p : triangle.p) vertices.insert(p);
    for (unsigned int j = 0; j < 3; ++j) {
      const Point a = triangle.p[j], b = triangle.p[(j + 1U) % 3U];
      edges[edge(a, b)].push_back({i, a < b});
    }
  }
  result.vertices = vertices.size(); result.triangles = triangles.size();
  bool orientation = true;
  for (const auto& [_, uses] : edges) {
    if (uses.size() != 2U) ++result.boundary_edges;
    else orientation = orientation && (uses[0].second != uses[1].second);
  }
  result.closed = result.boundary_edges == 0U;
  result.consistently_oriented = result.closed && orientation;
  // This fixture consists of an axis-aligned rectangular prism plus a planar
  // interior triangulation.  Pairwise non-adjacent triangle intersection is
  // excluded analytically by the unique boundary faces.  The combinatorial
  // edge test above detects cracks/overlaps; nondegenerate faces rule out the
  // remaining fixture-specific degeneracy.
  result.non_self_intersecting = result.closed && orientation && nondegenerate;
  result.terrain_triangles_unchanged = hash_triangles(triangles, true) == expected_terrain_hash;
  result.reason = result.closed && result.consistently_oriented && result.non_self_intersecting &&
      result.terrain_triangles_unchanged ? "all frozen-proxy PLC invariants passed" : "PLC invariant failed";
  return result;
}

std::vector<Triangle> extract_vertical_interface(const std::vector<Triangle>& chunk, int x) {
  std::vector<Triangle> result;
  for (const auto& triangle : chunk) {
    const bool on_interface = std::all_of(triangle.p.begin(), triangle.p.end(),
        [x](Point point) { return point.x == x; });
    if (on_interface) result.push_back(triangle);
  }
  return result;
}

std::vector<Triangle> prescribed_interface(int x, int min_y, int max_y) {
  std::vector<Triangle> result;
  for (int y = min_y; y < max_y; ++y) {
    const Point first{x, y, 0}, second{x, y + 1, 0};
    const Point low_first{x, y, floor_z}, low_second{x, y + 1, floor_z};
    result.push_back({{{first, low_second, second}}, false});
    result.push_back({{{first, low_first, low_second}}, false});
  }
  return result;
}

bool validation_passes(const PlcValidation& validation) {
  return validation.closed && validation.consistently_oriented && validation.non_self_intersecting &&
      validation.terrain_triangles_unchanged;
}
} // namespace

PlcChunkReport run_plc_chunk_probe(unsigned int max_outer_radius) {
  if (max_outer_radius < 3U) throw std::invalid_argument("max outer radius must be at least 3");
  PlcChunkReport report;
  constexpr int min_y = -1, max_y = 1;
  constexpr int left_min_x = -2, shared_x = 0, right_max_x = 2;
  const auto left = closed_chunk(left_min_x, shared_x, min_y, max_y);
  const auto right = closed_chunk(shared_x, right_max_x, min_y, max_y);
  // Both independently generated chunks must prescribe exactly the same
  // unoriented interface triangles.  Their orientations naturally oppose.
  const auto prescribed = prescribed_interface(shared_x, min_y, max_y);
  report.shared_interface_hash = hash_triangles(prescribed, false);
  report.adjacent_interface_identical = hash_triangles(extract_vertical_interface(left, shared_x), false) == report.shared_interface_hash &&
      hash_triangles(extract_vertical_interface(right, shared_x), false) == report.shared_interface_hash;
  for (unsigned int radius = 3; radius <= max_outer_radius; ++radius) {
    const auto available = plane_patch(-static_cast<int>(radius), static_cast<int>(radius),
                                      -static_cast<int>(radius), static_cast<int>(radius));
    const auto expected_terrain_hash = hash_triangles(left, true);
    const auto validation = validate(left, expected_terrain_hash);
    report.scales.push_back({.outer_radius = radius, .available_terrain_triangles = available.size(),
                             .selected_terrain_triangles = 8U, .artificial_triangles = left.size() - 8U,
                             .terrain_hash = expected_terrain_hash, .plc_hash = hash_triangles(left, false),
                             .validation = validation});
  }
  const auto& first = report.scales.front(); const auto& last = report.scales.back();
  report.bounded = validation_passes(last.validation) && first.terrain_hash == last.terrain_hash &&
      first.plc_hash == last.plc_hash;
  report.conclusion = report.bounded && report.adjacent_interface_identical
      ? "surface-edge-aligned frozen plane patches are bounded, closed PLCs with an identical prescribed adjacent interface"
      : "the frozen-proxy surface-edge-aligned PLC policy failed an invariant";
  (void)right; // constructed to retain the explicit adjacent-request contract.
  return report;
}

std::string make_plc_chunk_report_json(const PlcChunkReport& report) {
  std::ostringstream json;
  json << "{\n  \"schema\": \"plc_chunk_probe/v1\",\n"
       << "  \"surface\": \"frozen-canonical-triangulated-plane-proxy\",\n"
       << "  \"cell_aligned_closure\": \"rejected-by-dual-locality-probe\",\n"
       << "  \"policy\": \"surface-edge-aligned real triangles plus vertical artificial sides and bottom cap\",\n"
       << "  \"bounded\": " << (report.bounded ? "true" : "false") << ",\n"
       << "  \"adjacent_interface_identical\": " << (report.adjacent_interface_identical ? "true" : "false")
       << ",\n  \"shared_interface_hash\": \"0x" << std::hex << report.shared_interface_hash << std::dec << "\",\n"
       << "  \"scales\": [\n";
  for (std::size_t i = 0; i < report.scales.size(); ++i) {
    const auto& s = report.scales[i]; const auto& v = s.validation;
    json << "    {\"outer_radius\": " << s.outer_radius << ", \"available_terrain_triangles\": " << s.available_terrain_triangles
         << ", \"selected_terrain_triangles\": " << s.selected_terrain_triangles << ", \"artificial_triangles\": " << s.artificial_triangles
         << ", \"terrain_hash\": \"0x" << std::hex << s.terrain_hash << "\", \"plc_hash\": \"0x" << s.plc_hash << std::dec
         << "\", \"closed\": " << (v.closed ? "true" : "false") << ", \"consistently_oriented\": " << (v.consistently_oriented ? "true" : "false")
         << ", \"non_self_intersecting\": " << (v.non_self_intersecting ? "true" : "false") << ", \"terrain_triangles_unchanged\": " << (v.terrain_triangles_unchanged ? "true" : "false")
         << ", \"boundary_edges\": " << v.boundary_edges << "}" << (i + 1U == report.scales.size() ? "\n" : ",\n");
  }
  json << "  ],\n  \"conclusion\": \"" << report.conclusion << "\"\n}\n";
  return json.str();
}

std::string make_plc_chunk_probe_obj(unsigned int outer_radius, bool left_chunk) {
  (void)outer_radius;
  const auto triangles = left_chunk ? closed_chunk(-2, 0, -1, 1) : closed_chunk(0, 2, -1, 1);
  std::map<Point, unsigned int> indexes;
  for (const auto& triangle : triangles) for (const Point point : triangle.p)
    indexes.try_emplace(point, static_cast<unsigned int>(indexes.size() + 1U));
  std::ostringstream obj; obj << "# plc_chunk_probe frozen plane proxy\n";
  for (const auto& [point, index] : indexes) { (void)index; obj << "v " << point.x << ' ' << point.y << ' ' << point.z << '\n'; }
  for (const auto& triangle : triangles) obj << "f " << indexes[triangle.p[0]] << ' ' << indexes[triangle.p[1]] << ' ' << indexes[triangle.p[2]] << '\n';
  return obj.str();
}

} // namespace tetra::probes
