#include "tetra_probes/dual_locality_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace tetra::probes {
namespace {

// Coordinates are exact integers in twelfths of one root-cube edge.  This is
// the common coordinate system of the four-hexahedra construction, so shared
// primal edges cannot accidentally be merged by floating-point tolerance.
struct Point {
  int x{};
  int y{};
  int z{};
  friend auto operator<=>(const Point&, const Point&) = default;
};

struct Edge {
  Point first{};
  Point second{};
  friend auto operator<=>(const Edge&, const Edge&) = default;
};

struct Hexahedron {
  std::array<Point, 8> corners{};
  bool boundary{};
  Point centre_twelfths{};
};

constexpr std::array<std::array<unsigned int, 2>, 12> hexahedron_edges{{
    {{0U, 1U}}, {{0U, 2U}}, {{1U, 3U}}, {{2U, 3U}},
    {{4U, 5U}}, {{4U, 6U}}, {{5U, 7U}}, {{6U, 7U}},
    {{0U, 4U}}, {{1U, 5U}}, {{2U, 6U}}, {{3U, 7U}}}};

Point add(Point left, Point right) {
  return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Point scale(Point value, int multiplier) {
  return {value.x * multiplier, value.y * multiplier, value.z * multiplier};
}

Edge canonical_edge(Point first, Point second) {
  if (second < first) std::swap(first, second);
  return {first, second};
}

double coordinate(Point point, int component) {
  const int numerator = component == 0 ? point.x : component == 1 ? point.y : point.z;
  return static_cast<double>(numerator) / 12.0;
}

double field_value(LocalityField field, Point point) {
  const double x = coordinate(point, 0);
  const double y = coordinate(point, 1);
  const double z = coordinate(point, 2);
  if (field == LocalityField::plane)
    return x + 0.371 * y - 0.197 * z + 0.113;
  const double dx = x - 0.173;
  const double dy = y + 0.119;
  const double dz = z - 0.071;
  return dx * dx + dy * dy + dz * dz - 0.83 * 0.83;
}

bool edge_crosses(LocalityField field, Edge edge) {
  const double first = field_value(field, edge.first);
  const double second = field_value(field, edge.second);
  if (first == 0.0 || second == 0.0)
    throw std::logic_error("probe field unexpectedly evaluates to zero at a primal vertex");
  return (first < 0.0) != (second < 0.0);
}

std::array<Point, 4> tetrahedron(Point origin, std::array<int, 3> permutation) {
  constexpr std::array<Point, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const Point first = add(origin, axes[static_cast<std::size_t>(permutation[0])]);
  const Point second = add(first, axes[static_cast<std::size_t>(permutation[1])]);
  return {origin, first, second, add(second, axes[static_cast<std::size_t>(permutation[2])])};
}

std::vector<Hexahedron> make_uniform_root_complex(unsigned int radius) {
  if (radius == 0U) throw std::invalid_argument("domain radius must be positive");
  const int extent = static_cast<int>(radius);
  constexpr std::array<std::array<int, 3>, 6> permutations{{
      {{0, 1, 2}}, {{0, 2, 1}}, {{1, 0, 2}},
      {{1, 2, 0}}, {{2, 0, 1}}, {{2, 1, 0}}}};
  std::vector<Hexahedron> result;
  result.reserve(static_cast<std::size_t>(2 * extent) * static_cast<std::size_t>(2 * extent) *
                 static_cast<std::size_t>(2 * extent) * 24U);
  for (int z = -extent; z < extent; ++z) for (int y = -extent; y < extent; ++y)
    for (int x = -extent; x < extent; ++x) for (const auto permutation : permutations) {
      const auto tet = tetrahedron({x, y, z}, permutation);
      for (unsigned int owner = 0; owner < 4U; ++owner) {
        std::array<unsigned int, 3> others{};
        unsigned int output{};
        for (unsigned int vertex = 0; vertex < 4U; ++vertex)
          if (vertex != owner) others[output++] = vertex;
        const auto weighted = [&](std::initializer_list<std::pair<unsigned int, int>> terms) {
          Point answer{};
          for (const auto& [vertex, weight] : terms) answer = add(answer, scale(tet[vertex], weight));
          return answer;
        };
        Hexahedron hex;
        hex.corners = {{
            weighted({{owner, 12}}),
            weighted({{owner, 6}, {others[0], 6}}),
            weighted({{owner, 6}, {others[1], 6}}),
            weighted({{owner, 4}, {others[0], 4}, {others[1], 4}}),
            weighted({{owner, 6}, {others[2], 6}}),
            weighted({{owner, 4}, {others[0], 4}, {others[2], 4}}),
            weighted({{owner, 4}, {others[1], 4}, {others[2], 4}}),
            weighted({{0U, 3}, {1U, 3}, {2U, 3}, {3U, 3}})}};
        for (const Point corner : hex.corners) hex.centre_twelfths = add(hex.centre_twelfths, corner);
        // A cell is outer-boundary-adjacent if any primal vertex touches the
        // outer root-cube boundary.  It is an unambiguous finite-domain signal.
        for (const Point corner : hex.corners)
          hex.boundary = hex.boundary || std::abs(corner.x) == 12 * extent ||
              std::abs(corner.y) == 12 * extent || std::abs(corner.z) == 12 * extent;
        result.push_back(hex);
      }
    }
  return result;
}

bool inside_fixed_request(const Hexahedron& hex) {
  // centre_twelfths is the sum of eight points, so |sum| <= 48 represents
  // a centre coordinate in [-0.5, 0.5].
  return std::abs(hex.centre_twelfths.x) <= 48 && std::abs(hex.centre_twelfths.y) <= 48 &&
      std::abs(hex.centre_twelfths.z) <= 48;
}

std::uint64_t hash_cells(const std::vector<Hexahedron>& cells, const std::set<std::size_t>& selected) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const std::size_t index : selected) for (const Point point : cells[index].corners)
    for (const int coordinate : {point.x, point.y, point.z}) {
      hash ^= static_cast<std::uint32_t>(coordinate);
      hash *= 1099511628211ULL;
    }
  return hash;
}

std::string classification_name(LocalityClassification classification) {
  return classification == LocalityClassification::bounded ? "bounded" : "percolating";
}

} // namespace

std::string_view locality_field_name(LocalityField field) {
  return field == LocalityField::plane ? "plane" : "sphere";
}

LocalityFieldReport run_dual_locality_probe(LocalityField field, unsigned int max_domain_radius) {
  if (max_domain_radius < 2U)
    throw std::invalid_argument("max domain radius must be at least 2 to classify locality");
  LocalityFieldReport report;
  report.field = field;
  for (unsigned int radius = 1U; radius <= max_domain_radius; ++radius) {
    const auto cells = make_uniform_root_complex(radius);
    std::map<Edge, std::vector<std::size_t>> incident_cells;
    for (std::size_t cell = 0; cell < cells.size(); ++cell)
      for (const auto [first, second] : hexahedron_edges)
        incident_cells[canonical_edge(cells[cell].corners[first], cells[cell].corners[second])].push_back(cell);

    std::vector<std::vector<const std::vector<std::size_t>*>> owned(cells.size());
    std::size_t crossed_edges{};
    for (const auto& [edge, ring] : incident_cells) if (edge_crosses(field, edge)) {
      ++crossed_edges;
      const auto owner = *std::min_element(ring.begin(), ring.end());
      owned[owner].push_back(&ring);
    }

    std::set<std::size_t> closure;
    for (std::size_t cell = 0; cell < cells.size(); ++cell)
      if (inside_fixed_request(cells[cell])) closure.insert(cell);
    const std::size_t seed_cells = closure.size();
    bool changed{};
    do {
      changed = false;
      std::vector<std::size_t> additions;
      for (const std::size_t cell : closure)
        for (const auto* ring : owned[cell]) additions.insert(additions.end(), ring->begin(), ring->end());
      for (const std::size_t cell : additions) changed = closure.insert(cell).second || changed;
    } while (changed);

    LocalityScaleResult scale{.domain_radius = radius, .hexahedra = cells.size(),
                               .crossed_edges = crossed_edges, .seed_cells = seed_cells,
                               .closure_cells = closure.size(), .closure_hash = hash_cells(cells, closure)};
    std::set<const std::vector<std::size_t>*> closure_primitives;
    for (const std::size_t cell : closure) {
      scale.closure_touches_outer_boundary = scale.closure_touches_outer_boundary || cells[cell].boundary;
      for (const auto* ring : owned[cell]) closure_primitives.insert(ring);
    }
    scale.closure_crossed_edges = closure_primitives.size();
    for (const auto* ring : closure_primitives) {
      if (ring->size() == 3U) ++scale.closure_valence_3;
      if (ring->size() == 4U) ++scale.closure_valence_4;
      if (ring->size() == 6U) ++scale.closure_valence_6;
    }
    report.scales.push_back(scale);
  }
  const auto& penultimate = report.scales[report.scales.size() - 2U];
  const auto& last = report.scales.back();
  const bool stable = penultimate.closure_hash == last.closure_hash &&
      penultimate.closure_cells == last.closure_cells && !last.closure_touches_outer_boundary;
  report.classification = stable ? LocalityClassification::bounded : LocalityClassification::percolating;
  report.reason = stable
      ? "the two largest domains have identical fixed-point closure and it does not touch the outer boundary"
      : "the fixed-point closure changes with the outer domain or touches its boundary";
  return report;
}

std::string make_dual_locality_report_json(const std::vector<LocalityFieldReport>& reports) {
  std::ostringstream json;
  json << "{\n  \"schema\": \"dual_locality_probe/v1\",\n"
       << "  \"fixture\": \"uniform-root-freudenthal-six-tet-four-hex\",\n"
       << "  \"request\": {\"type\": \"fixed-cell-centre-cube\", \"min\": [-0.5,-0.5,-0.5], \"max\": [0.5,0.5,0.5]},\n"
       << "  \"closure_rule\": \"least fixed point of canonical crossed-edge owner output plus complete incident-cell ring support\",\n"
       << "  \"fields\": [\n";
  for (std::size_t index = 0; index < reports.size(); ++index) {
    const auto& field = reports[index];
    json << "    {\"field\": \"" << locality_field_name(field.field) << "\", \"classification\": \""
         << classification_name(field.classification) << "\", \"reason\": \"" << field.reason << "\", \"scales\": [\n";
    for (std::size_t scale_index = 0; scale_index < field.scales.size(); ++scale_index) {
      const auto& scale = field.scales[scale_index];
      json << "      {\"domain_radius\": " << scale.domain_radius << ", \"hexahedra\": " << scale.hexahedra
           << ", \"crossed_edges\": " << scale.crossed_edges << ", \"seed_cells\": " << scale.seed_cells
           << ", \"closure_cells\": " << scale.closure_cells << ", \"closure_crossed_edges\": " << scale.closure_crossed_edges
           << ", \"closure_valence_3\": " << scale.closure_valence_3 << ", \"closure_valence_4\": " << scale.closure_valence_4
           << ", \"closure_valence_6\": " << scale.closure_valence_6 << ", \"closure_touches_outer_boundary\": "
           << (scale.closure_touches_outer_boundary ? "true" : "false") << ", \"closure_hash\": \"0x" << std::hex
           << scale.closure_hash << std::dec << "\"}" << (scale_index + 1U == field.scales.size() ? "\n" : ",\n");
    }
    json << "    ]}" << (index + 1U == reports.size() ? "\n" : ",\n");
  }
  json << "  ]\n}\n";
  return json.str();
}

} // namespace tetra::probes
