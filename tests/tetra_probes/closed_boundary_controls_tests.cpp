#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <map>

namespace {
using Face=std::array<unsigned,3>;
using Edge=std::array<unsigned,2>;
struct Point { double x{},y{},z{}; };
Point operator-(const Point& a,const Point& b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Point cross(const Point& a,const Point& b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double dot(const Point& a,const Point& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }

Face sorted_face(Face face) { std::sort(face.begin(),face.end());return face; }
Edge sorted_edge(Edge edge) { std::sort(edge.begin(),edge.end());return edge; }

// A closed boundary is not "zero exposed faces": a single tetrahedron has
// four exposed faces and is the smallest valid closed volume.  This control
// checks the actual boundary complex: every boundary face is singly owned and
// every undirected boundary edge has exactly two incident faces.
bool is_oriented_closed_boundary(const std::array<Point,4>& vertices,
                                const std::vector<Face>& boundary) {
  std::map<Face,unsigned> faces;
  std::map<Edge,unsigned> edges;
  for(auto face:boundary) {
    if(++faces[sorted_face(face)]!=1U)return false;
    const auto& a=vertices[face[0]];
    const auto normal=cross(vertices[face[1]]-a,vertices[face[2]]-a);
    // The remaining tet vertex must be strictly on the interior side.  For
    // this small control the boundary is the source tet's complete boundary.
    unsigned opposite=0U;while(opposite==face[0]||opposite==face[1]||opposite==face[2])++opposite;
    if(dot(normal,vertices[opposite]-a)>=-1e-13)return false;
    for(unsigned edge=0;edge<3;++edge)
      ++edges[sorted_edge({{face[edge],face[(edge+1U)%3U]}})];
  }
  return !boundary.empty()&&std::all_of(edges.begin(),edges.end(),
      [](const auto& entry) { return entry.second==2U; });
}

std::array<Point,4> positive_tet() {
  return {{{0,0,0},{1,0,0},{0,1,0},{0,0,1}}};
}

std::vector<Face> tet_boundary() {
  // Outward orientation for the positive tetrahedron above.
  return {{{{0,2,1}},{{0,1,3}},{{0,3,2}},{{1,2,3}}}};
}
} // namespace

TEST_CASE("oriented closed-boundary contract accepts the positive single-tet control") {
  CHECK(is_oriented_closed_boundary(positive_tet(),tet_boundary()));
}

TEST_CASE("oriented closed-boundary contract rejects an open tetrahedron") {
  auto boundary=tet_boundary();
  boundary.pop_back();
  CHECK_FALSE(is_oriented_closed_boundary(positive_tet(),boundary));
}
