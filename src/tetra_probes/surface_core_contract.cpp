#include "tetra_probes/surface_core_contract.hpp"
#include "tetra_probes/exact_binary_predicates.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <numbers>
#include <set>

namespace tetra::probes {

bool is_semantically_coplanar(
    const std::array<std::uint64_t,4>& stable_vertex_ids,
    std::span<const ExactAffinePlaneProvenance> planes) {
  return std::ranges::any_of(planes,[&](const auto& plane) {
    return std::ranges::all_of(stable_vertex_ids,[&](std::uint64_t id) {
      return std::binary_search(plane.vertex_ids.begin(),plane.vertex_ids.end(),id);
    });
  });
}

namespace {

using Edge = std::array<std::uint32_t, 2>;
using Face = std::array<std::uint32_t, 3>;
using Tet = std::array<std::uint32_t, 4>;

[[nodiscard]] Edge edge(std::uint32_t a, std::uint32_t b) {
  if (b < a) std::swap(a, b);
  return {a, b};
}
[[nodiscard]] Face face(Face value) { std::sort(value.begin(), value.end()); return value; }
[[nodiscard]] Tet tet(Tet value) { std::sort(value.begin(), value.end()); return value; }
[[nodiscard]] Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
[[nodiscard]] double dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
[[nodiscard]] double length(Vec3 a) { return std::sqrt(dot(a,a)); }
[[nodiscard]] bool finite(Vec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
[[nodiscard]] bool strictly_inside_closed_surface(
    const std::vector<Vec3>& vertices, const std::vector<std::array<std::uint32_t, 3>>& faces, Vec3 point) {
  // Signed solid angle is independent of triangle winding after abs().  This
  // avoids the ray-through-edge ambiguity that would otherwise make an input
  // contract depend on arbitrary vertex alignment.
  double winding{};
  for (const auto triangle:faces) {
    const Vec3 a=vertices[triangle[0]]-point, b=vertices[triangle[1]]-point,
               c=vertices[triangle[2]]-point;
    const double la=std::sqrt(dot(a,a)), lb=std::sqrt(dot(b,b)), lc=std::sqrt(dot(c,c));
    const double numerator=dot(a,cross(b,c));
    const double denominator=la*lb*lc+dot(a,b)*lc+dot(b,c)*la+dot(c,a)*lb;
    winding+=2.0*std::atan2(numerator,denominator);
  }
  constexpr double full_sphere=12.56637061435917295385;
  return std::abs(std::abs(winding)-full_sphere) < 1.0e-7;
}
[[nodiscard]] bool strict_segment_triangle_intersection(
    Vec3 start, Vec3 end, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 direction=end-start, first=b-a, second=c-a, p=cross(direction,second);
  const double determinant=dot(first,p);
  constexpr double epsilon=1.0e-12;
  if (std::abs(determinant)<=epsilon) return false;
  const double inverse=1.0/determinant;
  const Vec3 offset=start-a;
  const double u=dot(offset,p)*inverse;
  if (u<=epsilon || u>=1.0-epsilon) return false;
  const Vec3 q=cross(offset,first);
  const double v=dot(direction,q)*inverse;
  if (v<=epsilon || u+v>=1.0-epsilon) return false;
  const double t=dot(second,q)*inverse;
  return t>epsilon && t<1.0-epsilon;
}
[[nodiscard]] bool strict_coplanar_triangles_intersect(const std::array<Vec3,3>& left,
                                                        const std::array<Vec3,3>& right, Vec3 normal) {
  const Vec3 magnitude{std::abs(normal.x),std::abs(normal.y),std::abs(normal.z)};
  const unsigned dropped=magnitude.x>=magnitude.y && magnitude.x>=magnitude.z ? 0U : (magnitude.y>=magnitude.z ? 1U : 2U);
  const auto project=[dropped](Vec3 p) { return dropped==0U?std::array<double,2>{{p.y,p.z}}:(dropped==1U?std::array<double,2>{{p.x,p.z}}:std::array<double,2>{{p.x,p.y}}); };
  const auto cross2=[](const auto& a,const auto& b,const auto& c) { return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]); };
  constexpr double epsilon=1.0e-12;
  const auto inside=[&](Vec3 point,const std::array<Vec3,3>& triangle) {
    const auto p=project(point), a=project(triangle[0]), b=project(triangle[1]), c=project(triangle[2]);
    const double ab=cross2(a,b,p), bc=cross2(b,c,p), ca=cross2(c,a,p);
    return (ab>epsilon&&bc>epsilon&&ca>epsilon)||(ab<-epsilon&&bc<-epsilon&&ca<-epsilon);
  };
  const auto proper_segments=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
    const auto A=project(a),B=project(b),C=project(c),D=project(d);
    const double abc=cross2(A,B,C), abd=cross2(A,B,D), cda=cross2(C,D,A), cdb=cross2(C,D,B);
    return ((abc>epsilon&&abd<-epsilon)||(abc<-epsilon&&abd>epsilon)) &&
        ((cda>epsilon&&cdb<-epsilon)||(cda<-epsilon&&cdb>epsilon));
  };
  for (std::size_t i=0; i<3U; ++i) for (std::size_t j=0; j<3U; ++j)
    if (proper_segments(left[i],left[(i+1U)%3U],right[j],right[(j+1U)%3U])) return true;
  for (const auto point:left) if (inside(point,right)) return true;
  for (const auto point:right) if (inside(point,left)) return true;
  return false;
}
[[nodiscard]] bool strict_triangles_intersect(const std::array<Vec3,3>& left,
                                              const std::array<Vec3,3>& right) {
  const Vec3 left_normal=cross(left[1]-left[0],left[2]-left[0]);
  const Vec3 right_normal=cross(right[1]-right[0],right[2]-right[0]);
  constexpr double epsilon=1.0e-12;
  if (length(cross(left_normal,right_normal))<=epsilon*length(left_normal)*length(right_normal) &&
      std::abs(dot(left_normal,right[0]-left[0]))<=epsilon*length(left_normal))
    return strict_coplanar_triangles_intersect(left,right,left_normal);
  for (std::size_t edge_index=0; edge_index<3U; ++edge_index) {
    if (strict_segment_triangle_intersection(left[edge_index],left[(edge_index+1U)%3U],right[0],right[1],right[2])) return true;
    if (strict_segment_triangle_intersection(right[edge_index],right[(edge_index+1U)%3U],left[0],left[1],left[2])) return true;
  }
  return false;
}
[[nodiscard]] double point_segment_distance_squared(Vec3 point,Vec3 a,Vec3 b) {
  const Vec3 direction=b-a;
  const double squared=dot(direction,direction);
  if(squared==0.0)return dot(point-a,point-a);
  const double t=std::clamp(dot(point-a,direction)/squared,0.0,1.0);
  const Vec3 nearest=a+direction*t;
  return dot(point-nearest,point-nearest);
}
[[nodiscard]] double segment_segment_distance_squared(Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
  const Vec3 u=b-a,v=d-c,w=a-c;
  const double uu=dot(u,u),uv=dot(u,v),vv=dot(v,v),uw=dot(u,w),vw=dot(v,w);
  const double denominator=uu*vv-uv*uv;
  double s{},t{};
  if(denominator>1.0e-30*std::max(1.0,uu*vv)) {
    s=std::clamp((uv*vw-vv*uw)/denominator,0.0,1.0);
    t=std::clamp((uu*vw-uv*uw)/denominator,0.0,1.0);
    // Clamping one parameter changes the unconstrained optimum for the other.
    if(s==0.0||s==1.0)t=std::clamp((uv*s+vw)/vv,0.0,1.0);
    if(t==0.0||t==1.0)s=std::clamp((uv*t-uw)/uu,0.0,1.0);
  } else {
    s=0.0;t=vv==0.0?0.0:std::clamp(vw/vv,0.0,1.0);
  }
  const Vec3 delta=w+u*s-v*t;
  return dot(delta,delta);
}
[[nodiscard]] double point_triangle_distance_squared(
    Vec3 point,const std::array<Vec3,3>& triangle) {
  const Vec3 ab=triangle[1]-triangle[0],ac=triangle[2]-triangle[0],ap=point-triangle[0];
  const double d1=dot(ab,ap),d2=dot(ac,ap);
  if(d1<=0.0&&d2<=0.0)return dot(ap,ap);
  const Vec3 bp=point-triangle[1];const double d3=dot(ab,bp),d4=dot(ac,bp);
  if(d3>=0.0&&d4<=d3)return dot(bp,bp);
  const double vc=d1*d4-d3*d2;
  if(vc<=0.0&&d1>=0.0&&d3<=0.0) {
    const double t=d1/(d1-d3);return point_segment_distance_squared(point,triangle[0],triangle[1]);
  }
  const Vec3 cp=point-triangle[2];const double d5=dot(ab,cp),d6=dot(ac,cp);
  if(d6>=0.0&&d5<=d6)return dot(cp,cp);
  const double vb=d5*d2-d1*d6;
  if(vb<=0.0&&d2>=0.0&&d6<=0.0)
    return point_segment_distance_squared(point,triangle[0],triangle[2]);
  const double va=d3*d6-d5*d4;
  if(va<=0.0&&(d4-d3)>=0.0&&(d5-d6)>=0.0)
    return point_segment_distance_squared(point,triangle[1],triangle[2]);
  const double inverse=1.0/(va+vb+vc);
  const Vec3 closest=triangle[0]+ab*(vb*inverse)+ac*(vc*inverse);
  return dot(point-closest,point-closest);
}
[[nodiscard]] double triangle_distance_squared(const std::array<Vec3,3>& left,
                                               const std::array<Vec3,3>& right) {
  if(strict_triangles_intersect(left,right))return 0.0;
  double result=std::numeric_limits<double>::infinity();
  for(const auto point:left)result=std::min(result,point_triangle_distance_squared(point,right));
  for(const auto point:right)result=std::min(result,point_triangle_distance_squared(point,left));
  for(unsigned i=0U;i<3U;++i)for(unsigned j=0U;j<3U;++j)
    result=std::min(result,segment_segment_distance_squared(
        left[i],left[(i+1U)%3U],right[j],right[(j+1U)%3U]));
  return result;
}
[[nodiscard]] bool strict_tetrahedra_overlap_impl(const std::array<Vec3,4>& a,
                                                   const std::array<Vec3,4>& b) {
  for (std::size_t axis=0; axis<3U; ++axis) {
    const auto coordinate=[axis](Vec3 p) { return axis==0U?p.x:(axis==1U?p.y:p.z); };
    double amin=coordinate(a[0]), amax=amin, bmin=coordinate(b[0]), bmax=bmin;
    for (std::size_t i=1; i<4U; ++i) { amin=std::min(amin,coordinate(a[i])); amax=std::max(amax,coordinate(a[i])); bmin=std::min(bmin,coordinate(b[i])); bmax=std::max(bmax,coordinate(b[i])); }
    if (amax<=bmin+1.0e-12 || bmax<=amin+1.0e-12) return false;
  }
  constexpr std::array<std::array<unsigned int,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  constexpr std::array<std::array<unsigned int,2>,6> edges{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::vector<Vec3> axes;
  for (const auto f:faces) { axes.push_back(cross(a[f[1]]-a[f[0]],a[f[2]]-a[f[0]])); axes.push_back(cross(b[f[1]]-b[f[0]],b[f[2]]-b[f[0]])); }
  for (const auto ea:edges) for (const auto eb:edges) axes.push_back(cross(a[ea[1]]-a[ea[0]],b[eb[1]]-b[eb[0]]));
  for (const auto axis:axes) {
    if (length(axis)<=1.0e-15) continue;
    double amin=dot(a[0],axis),amax=amin,bmin=dot(b[0],axis),bmax=bmin;
    for (std::size_t i=1; i<4U; ++i) { amin=std::min(amin,dot(a[i],axis)); amax=std::max(amax,dot(a[i],axis)); bmin=std::min(bmin,dot(b[i],axis)); bmax=std::max(bmax,dot(b[i],axis)); }
    const double tolerance=1.0e-12*std::max({1.0,std::abs(amin),std::abs(amax),std::abs(bmin),std::abs(bmax)});
    if (amax<=bmin+tolerance || bmax<=amin+tolerance) return false;
  }
  return true;
}

struct TetrahedronBounds {
  Vec3 minimum{};
  Vec3 maximum{};
};

[[nodiscard]] TetrahedronBounds tetrahedron_bounds(
    const std::array<Vec3,4>& tetrahedron) {
  TetrahedronBounds bounds{tetrahedron[0],tetrahedron[0]};
  for(std::size_t corner=1U;corner<tetrahedron.size();++corner) {
    const auto point=tetrahedron[corner];
    bounds.minimum.x=std::min(bounds.minimum.x,point.x);
    bounds.minimum.y=std::min(bounds.minimum.y,point.y);
    bounds.minimum.z=std::min(bounds.minimum.z,point.z);
    bounds.maximum.x=std::max(bounds.maximum.x,point.x);
    bounds.maximum.y=std::max(bounds.maximum.y,point.y);
    bounds.maximum.z=std::max(bounds.maximum.z,point.z);
  }
  return bounds;
}

[[nodiscard]] bool bounds_overlap(const TetrahedronBounds& first,
                                  const TetrahedronBounds& second) {
  return first.maximum.x>second.minimum.x&&second.maximum.x>first.minimum.x&&
      first.maximum.y>second.minimum.y&&second.maximum.y>first.minimum.y&&
      first.maximum.z>second.minimum.z&&second.maximum.z>first.minimum.z;
}

[[nodiscard]] TetrahedronBounds triangle_bounds(
    const std::array<Vec3,3>& triangle) {
  TetrahedronBounds bounds{triangle[0],triangle[0]};
  for(std::size_t corner=1U;corner<triangle.size();++corner) {
    const auto point=triangle[corner];
    bounds.minimum.x=std::min(bounds.minimum.x,point.x);
    bounds.minimum.y=std::min(bounds.minimum.y,point.y);
    bounds.minimum.z=std::min(bounds.minimum.z,point.z);
    bounds.maximum.x=std::max(bounds.maximum.x,point.x);
    bounds.maximum.y=std::max(bounds.maximum.y,point.y);
    bounds.maximum.z=std::max(bounds.maximum.z,point.z);
  }
  return bounds;
}

[[nodiscard]] double bounds_distance_squared(const TetrahedronBounds& first,
                                              const TetrahedronBounds& second) {
  const auto axis_distance=[](double first_minimum,double first_maximum,
                              double second_minimum,double second_maximum) {
    if(first_maximum<second_minimum)return second_minimum-first_maximum;
    if(second_maximum<first_minimum)return first_minimum-second_maximum;
    return 0.0;
  };
  const double x=axis_distance(first.minimum.x,first.maximum.x,
                               second.minimum.x,second.maximum.x);
  const double y=axis_distance(first.minimum.y,first.maximum.y,
                               second.minimum.y,second.maximum.y);
  const double z=axis_distance(first.minimum.z,first.maximum.z,
                               second.minimum.z,second.maximum.z);
  return x*x+y*y+z*z;
}

}  // namespace

FrozenFacetSplit split_frozen_facet(
    FrozenFacetIdentity parent, FacetPreservationMode mode,
    std::uint64_t local_chunk, std::uint64_t neighbour_chunk) {
  std::sort(parent.vertex_ids.begin(), parent.vertex_ids.end());
  FrozenFacetSplit result; result.parent=parent; result.mode=mode;
  result.owner_chunk=std::min(local_chunk,neighbour_chunk);
  const auto point=[](std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t d) {
    return FacetBarycentricPoint{{a,b,c},d};
  };
  const auto append=[&](std::array<FacetBarycentricPoint,3> corners) {
    result.subfaces.push_back({parent,corners,static_cast<std::uint32_t>(result.subfaces.size()),result.owner_chunk,
        local_chunk==result.owner_chunk});
  };
  if (mode==FacetPreservationMode::literal) {
    append({{point(1,0,0,1),point(0,1,0,1),point(0,0,1,1)}});
  } else {
    const auto a=point(1,0,0,1), b=point(0,1,0,1), c=point(0,0,1,1);
    const auto ab=point(1,1,0,2), bc=point(0,1,1,2), ca=point(1,0,1,2);
    append({{a,ab,ca}}); append({{ab,b,bc}}); append({{ca,bc,c}}); append({{ab,bc,ca}});
  }
  return result;
}

bool validate_frozen_facet_split(const FrozenFacetSplit& split) {
  if (split.subfaces.empty()) return false;
  FrozenFacetIdentity canonical_parent=split.parent;
  std::sort(canonical_parent.vertex_ids.begin(),canonical_parent.vertex_ids.end());
  constexpr std::uint64_t max_common_denominator=1U<<20U;
  std::uint64_t denominator=1U;
  const auto valid_point=[](const FacetBarycentricPoint& point) {
    if (point.denominator==0U) return false;
    const auto sum=static_cast<unsigned __int128>(point.numerator[0])+point.numerator[1]+point.numerator[2];
    if (sum!=point.denominator) return false;
    return std::gcd(std::gcd(point.numerator[0],point.numerator[1]),std::gcd(point.numerator[2],point.denominator))==1U;
  };
  for(const auto& subface:split.subfaces) for(const auto& point:subface.corners) {
    if(!valid_point(point)) return false;
    const auto divisor=std::gcd(denominator,static_cast<std::uint64_t>(point.denominator));
    if (denominator/divisor>max_common_denominator/point.denominator) return false;
    denominator=denominator/divisor*point.denominator;
  }
  const auto coordinate=[denominator](const FacetBarycentricPoint& point) {
    const auto scale=denominator/point.denominator;
    return std::array<std::int64_t,2>{{static_cast<std::int64_t>(point.numerator[1]*scale),static_cast<std::int64_t>(point.numerator[2]*scale)}};
  };
  const auto orient=[](const auto& a,const auto& b,const auto& c) {
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
  };
  std::int64_t summed_area{};
  std::vector<std::array<std::array<std::int64_t,2>,3>> triangles;
  std::set<std::array<std::array<std::int64_t,2>,3>> unique_triangles;
  triangles.reserve(split.subfaces.size());
  for(std::size_t i=0;i<split.subfaces.size();++i) {
    const auto& subface=split.subfaces[i]; FrozenFacetIdentity id=subface.parent;
    std::sort(id.vertex_ids.begin(),id.vertex_ids.end());
    if(id!=canonical_parent || subface.ordinal!=i || subface.owner_chunk!=split.owner_chunk) return false;
    const std::array<std::array<std::int64_t,2>,3> triangle{{coordinate(subface.corners[0]),coordinate(subface.corners[1]),coordinate(subface.corners[2])}};
    const auto area=orient(triangle[0],triangle[1],triangle[2]);
    if(area<=0) return false;
    auto canonical_triangle=triangle; std::sort(canonical_triangle.begin(),canonical_triangle.end());
    if(!unique_triangles.insert(canonical_triangle).second) return false;
    summed_area+=area; triangles.push_back(triangle);
  }
  if(summed_area!=static_cast<std::int64_t>(denominator*denominator)) return false;
  // Strict edge crossings or strict containment are exactly the positive-area
  // overlap cases for triangles already known to lie inside the parent.
  for(std::size_t i=0;i<triangles.size();++i)for(std::size_t j=i+1;j<triangles.size();++j) {
    const auto& a=triangles[i]; const auto& b=triangles[j];
    for(std::size_t x=0;x<3;++x)for(std::size_t y=0;y<3;++y) {
      const auto ab1=orient(a[x],a[(x+1U)%3U],b[y]), ab2=orient(a[x],a[(x+1U)%3U],b[(y+1U)%3U]);
      const auto ba1=orient(b[y],b[(y+1U)%3U],a[x]), ba2=orient(b[y],b[(y+1U)%3U],a[(x+1U)%3U]);
      if(((ab1>0&&ab2<0)||(ab1<0&&ab2>0))&&((ba1>0&&ba2<0)||(ba1<0&&ba2>0))) return false;
    }
    for(const auto& point:a) if(orient(b[0],b[1],point)>0&&orient(b[1],b[2],point)>0&&orient(b[2],b[0],point)>0) return false;
    for(const auto& point:b) if(orient(a[0],a[1],point)>0&&orient(a[1],a[2],point)>0&&orient(a[2],a[0],point)>0) return false;
  }
  return true;
}

Vec3 evaluate_facet_barycentric(
    std::array<FrozenFacetVertex,3> parent, const FacetBarycentricPoint& point) {
  if(point.denominator==0U) return {};
  std::sort(parent.begin(),parent.end(),[](const auto& a,const auto& b) { return a.id<b.id; });
  if(parent[0].id==parent[1].id||parent[1].id==parent[2].id) return {};
  return (parent[0].position*static_cast<double>(point.numerator[0])+parent[1].position*static_cast<double>(point.numerator[1])+parent[2].position*static_cast<double>(point.numerator[2]))/static_cast<double>(point.denominator);
}

const char* surface_core_input_failure_name(SurfaceCoreInputFailure failure) {
  switch (failure) {
    case SurfaceCoreInputFailure::none: return "none";
    case SurfaceCoreInputFailure::resource_limit: return "resource_limit";
    case SurfaceCoreInputFailure::empty_outer_boundary: return "empty_outer_boundary";
    case SurfaceCoreInputFailure::empty_core: return "empty_core";
    case SurfaceCoreInputFailure::non_finite_vertex: return "non_finite_vertex";
    case SurfaceCoreInputFailure::invalid_index: return "invalid_index";
    case SurfaceCoreInputFailure::repeated_vertex: return "repeated_vertex";
    case SurfaceCoreInputFailure::degenerate_outer_face: return "degenerate_outer_face";
    case SurfaceCoreInputFailure::outer_quality_below_contract: return "outer_quality_below_contract";
    case SurfaceCoreInputFailure::duplicate_outer_face: return "duplicate_outer_face";
    case SurfaceCoreInputFailure::outer_not_closed_two_manifold: return "outer_not_closed_two_manifold";
    case SurfaceCoreInputFailure::outer_inconsistent_orientation: return "outer_inconsistent_orientation";
    case SurfaceCoreInputFailure::outer_self_intersection: return "outer_self_intersection";
    case SurfaceCoreInputFailure::degenerate_core_tetrahedron: return "degenerate_core_tetrahedron";
    case SurfaceCoreInputFailure::duplicate_core_tetrahedron: return "duplicate_core_tetrahedron";
    case SurfaceCoreInputFailure::core_not_two_manifold: return "core_not_two_manifold";
    case SurfaceCoreInputFailure::core_inconsistent_orientation: return "core_inconsistent_orientation";
    case SurfaceCoreInputFailure::core_not_strictly_nested: return "core_not_strictly_nested";
    case SurfaceCoreInputFailure::core_touches_or_intersects_outer: return "core_touches_or_intersects_outer";
    case SurfaceCoreInputFailure::invalid_facet_contract: return "invalid_facet_contract";
  }
  return "unknown";
}

const char* surface_core_construction_failure_name(SurfaceCoreConstructionFailure failure) {
  switch (failure) {
    case SurfaceCoreConstructionFailure::none: return "none";
    case SurfaceCoreConstructionFailure::rejected_input_contract: return "rejected_input_contract";
    case SurfaceCoreConstructionFailure::missing_or_invalid_correspondence: return "missing_or_invalid_correspondence";
    case SurfaceCoreConstructionFailure::core_front_not_homologous: return "core_front_not_homologous";
    case SurfaceCoreConstructionFailure::geometry_gate_rejected: return "geometry_gate_rejected";
    case SurfaceCoreConstructionFailure::quality_gate_rejected: return "quality_gate_rejected";
  }
  return "unknown";
}

SurfaceCoreTransitionContract validate_surface_core_transition_input(
    const SurfaceCoreTransitionInput& input) {
  SurfaceCoreTransitionContract result;
  const auto fail = [&](SurfaceCoreInputFailure failure, std::size_t index) {
    result.failure = failure; result.failing_element = index; return result;
  };
  if (input.vertices.size() > input.maximum_vertices ||
      input.outer_faces.size() > input.maximum_outer_faces ||
      input.retained_core_tetrahedra.size() > input.maximum_core_tetrahedra ||
      !std::isfinite(input.coordinate_scale) || input.coordinate_scale <= 0.0 ||
      !std::isfinite(input.minimum_outer_triangle_angle_degrees) ||
      input.minimum_outer_triangle_angle_degrees < 0.0 ||
      input.minimum_outer_triangle_angle_degrees >= 180.0)
    return fail(SurfaceCoreInputFailure::resource_limit, 0U);
  if (input.outer_faces.empty()) return fail(SurfaceCoreInputFailure::empty_outer_boundary, 0U);
  if (input.retained_core_tetrahedra.empty()) return fail(SurfaceCoreInputFailure::empty_core, 0U);
  if (!input.stable_vertex_ids.empty() && input.stable_vertex_ids.size()!=input.vertices.size())
    return fail(SurfaceCoreInputFailure::invalid_facet_contract,0U);
  std::set<std::uint64_t> stable_ids;
  for(std::size_t i=0;i<input.vertices.size();++i)
    if(!stable_ids.insert(input.stable_vertex_ids.empty()?static_cast<std::uint64_t>(i):input.stable_vertex_ids[i]).second)
      return fail(SurfaceCoreInputFailure::invalid_facet_contract,i);
  for(std::size_t plane_index=0U;plane_index<input.exact_affine_planes.size();++plane_index) {
    const auto& declared=input.exact_affine_planes[plane_index];
    const auto& plane=declared.vertex_ids;
    if(declared.construction.axis>=3U||declared.construction.denominator==0U||
       plane.size()<4U||!std::is_sorted(plane.begin(),plane.end())||
       std::adjacent_find(plane.begin(),plane.end())!=plane.end()||
       !std::ranges::all_of(plane,[&](std::uint64_t id) { return stable_ids.contains(id); }))
      return fail(SurfaceCoreInputFailure::invalid_facet_contract,plane_index);
  }
  if(!input.outer_parent_facets.empty()&&input.outer_parent_facets.size()!=input.outer_faces.size())
    return fail(SurfaceCoreInputFailure::invalid_facet_contract,0U);
  for (std::size_t i=0; i<input.vertices.size(); ++i)
    if (!finite(input.vertices[i])) return fail(SurfaceCoreInputFailure::non_finite_vertex, i);

  std::set<Face> unique_outer;
  std::map<Edge, std::vector<int>> outer_edges;
  for (std::size_t i=0; i<input.outer_faces.size(); ++i) {
    const auto triangle=input.outer_faces[i];
    for (const auto id:triangle) if (id >= input.vertices.size()) return fail(SurfaceCoreInputFailure::invalid_index, i);
    const auto key=face(triangle);
    if(!input.outer_parent_facets.empty()) {
      auto expected=FrozenFacetIdentity{{
        input.stable_vertex_ids.empty()?triangle[0]:input.stable_vertex_ids[triangle[0]],
        input.stable_vertex_ids.empty()?triangle[1]:input.stable_vertex_ids[triangle[1]],
        input.stable_vertex_ids.empty()?triangle[2]:input.stable_vertex_ids[triangle[2]]}};
      std::sort(expected.vertex_ids.begin(),expected.vertex_ids.end()); auto given=input.outer_parent_facets[i].identity;
      std::sort(given.vertex_ids.begin(),given.vertex_ids.end());
      if(given!=expected) return fail(SurfaceCoreInputFailure::invalid_facet_contract,i);
    }
    if (key[0] == key[1] || key[1] == key[2]) return fail(SurfaceCoreInputFailure::repeated_vertex, i);
    const auto normal=cross(input.vertices[triangle[1]]-input.vertices[triangle[0]],
                            input.vertices[triangle[2]]-input.vertices[triangle[0]]);
    // Nonzero area is a validity condition.  A scale-relative area floor is
    // a mesh-quality policy and must not decide whether Wang output may be
    // published.
    if (dot(normal,normal) == 0.0) return fail(SurfaceCoreInputFailure::degenerate_outer_face, i);
    for (std::size_t corner=0; corner<3U; ++corner) {
      const Vec3 first=input.vertices[triangle[(corner+1U)%3U]]-input.vertices[triangle[corner]];
      const Vec3 second=input.vertices[triangle[(corner+2U)%3U]]-input.vertices[triangle[corner]];
      const double cosine=std::clamp(dot(first,second)/(length(first)*length(second)),-1.0,1.0);
      const double degrees=std::acos(cosine)*180.0/std::numbers::pi;
      if (i==0U&&corner==0U) result.minimum_outer_triangle_angle_degrees=degrees;
      else result.minimum_outer_triangle_angle_degrees=std::min(result.minimum_outer_triangle_angle_degrees,degrees);
    }
    if (!unique_outer.insert(key).second) return fail(SurfaceCoreInputFailure::duplicate_outer_face, i);
    const auto add_edge=[&](std::uint32_t first,std::uint32_t second) {
      outer_edges[edge(first,second)].push_back(first<second ? 1 : -1);
    };
    add_edge(triangle[0],triangle[1]);
    add_edge(triangle[1],triangle[2]);
    add_edge(triangle[2],triangle[0]);
  }
  for (const auto& [unused, uses]:outer_edges) {
    static_cast<void>(unused);
    if (uses.size() == 1U) ++result.outer_boundary_edges;
    else if (uses.size() != 2U) ++result.outer_nonmanifold_edges;
    else if (uses[0]==uses[1]) return fail(SurfaceCoreInputFailure::outer_inconsistent_orientation, 0U);
  }
  if (result.outer_boundary_edges != 0U || result.outer_nonmanifold_edges != 0U)
    return fail(SurfaceCoreInputFailure::outer_not_closed_two_manifold, 0U);
  for (std::size_t left=0; left<input.outer_faces.size(); ++left)
    for (std::size_t right=left+1U; right<input.outer_faces.size(); ++right) {
      std::size_t shared{};
      for (const auto a:input.outer_faces[left]) for (const auto b:input.outer_faces[right])
        shared += a==b ? 1U : 0U;
      if (shared>=2U) continue;
      const auto points=[&](const auto& triangle) {
        return std::array<Vec3,3>{{input.vertices[triangle[0]],input.vertices[triangle[1]],input.vertices[triangle[2]]}};
      };
      if (strict_triangles_intersect(points(input.outer_faces[left]),points(input.outer_faces[right])))
        { result.related_element=right; return fail(SurfaceCoreInputFailure::outer_self_intersection, left); }
    }

  struct CoreFaceUse { std::uint32_t opposite{}; int sign{}; };
  std::set<Tet> unique_core;
  std::map<Face, std::vector<CoreFaceUse>> core_faces;
  for (std::size_t i=0; i<input.retained_core_tetrahedra.size(); ++i) {
    const auto cell=input.retained_core_tetrahedra[i];
    for (const auto id:cell) if (id >= input.vertices.size()) return fail(SurfaceCoreInputFailure::invalid_index, i);
    const auto key=tet(cell);
    if (key[0]==key[1] || key[1]==key[2] || key[2]==key[3]) return fail(SurfaceCoreInputFailure::repeated_vertex, i);
    if (exact_orientation_3d(input.vertices[cell[0]],input.vertices[cell[1]],
                             input.vertices[cell[2]],input.vertices[cell[3]])==
        ExactPredicateSign::zero)
      return fail(SurfaceCoreInputFailure::degenerate_core_tetrahedron, i);
    if (!unique_core.insert(key).second) return fail(SurfaceCoreInputFailure::duplicate_core_tetrahedron, i);
    for (std::size_t opposite=0; opposite<4U; ++opposite) {
      Face oriented{}; std::size_t cursor{};
      for (std::size_t vertex=0; vertex<4U; ++vertex) if (vertex != opposite) oriented[cursor++]=cell[vertex];
      const auto canonical=face(oriented);
      const auto normal=cross(input.vertices[canonical[1]]-input.vertices[canonical[0]],
                              input.vertices[canonical[2]]-input.vertices[canonical[0]]);
      const double side=dot(normal,input.vertices[cell[opposite]]-input.vertices[canonical[0]]);
      // Shared faces must have one tetrahedron on each geometric side.  This
      // deliberately does not trust the caller's local vertex ordering.
      const int sign=side > 0.0 ? 1 : -1;
      core_faces[canonical].push_back({cell[opposite], sign});
    }
  }
  for (const auto& [unused, uses]:core_faces) {
    static_cast<void>(unused);
    if (uses.size()==1U) ++result.core_boundary_faces;
    else if (uses.size()!=2U) ++result.core_nonmanifold_faces;
    else if (uses[0].sign == uses[1].sign) return fail(SurfaceCoreInputFailure::core_inconsistent_orientation, 0U);
  }
  if (result.core_nonmanifold_faces != 0U) return fail(SurfaceCoreInputFailure::core_not_two_manifold, 0U);
  if(!input.core_parent_facets.empty()) {
    std::set<FrozenFacetIdentity> expected, given;
    for(const auto& [core_face,uses]:core_faces) if(uses.size()==1U) {
      FrozenFacetIdentity id{{
        input.stable_vertex_ids.empty()?core_face[0]:input.stable_vertex_ids[core_face[0]],
        input.stable_vertex_ids.empty()?core_face[1]:input.stable_vertex_ids[core_face[1]],
        input.stable_vertex_ids.empty()?core_face[2]:input.stable_vertex_ids[core_face[2]]}};
      std::sort(id.vertex_ids.begin(),id.vertex_ids.end()); expected.insert(id);
    }
    for(const auto& parent:input.core_parent_facets) { auto id=parent.identity;std::sort(id.vertex_ids.begin(),id.vertex_ids.end());given.insert(id); }
    if(expected!=given||given.size()!=input.core_parent_facets.size()) return fail(SurfaceCoreInputFailure::invalid_facet_contract,0U);
  }
  std::set<std::uint32_t> unique_core_vertices;
  for(const auto cell:input.retained_core_tetrahedra)
    unique_core_vertices.insert(cell.begin(),cell.end());
  for(const auto vertex:unique_core_vertices)
    if(!strictly_inside_closed_surface(input.vertices,input.outer_faces,
                                       input.vertices[vertex]))
      return fail(SurfaceCoreInputFailure::core_not_strictly_nested,vertex);
  // A vertex-only nesting test is unsound for a concave outer PLC: a core
  // tetrahedron can exit and re-enter the enclosed volume between vertices.
  // Keep a clearance proportional to the declared coordinate scale.  This
  // exceeds the output validator's volume tolerance for the tested terrain
  // family and rejects touching interfaces before recovery can create slivers.
  const double required_clearance=input.coordinate_scale*1.0e-10;
  std::vector<TetrahedronBounds> outer_bounds;
  outer_bounds.reserve(input.outer_faces.size());
  for(const auto outer:input.outer_faces)
    outer_bounds.push_back(triangle_bounds({{input.vertices[outer[0]],
                                             input.vertices[outer[1]],
                                             input.vertices[outer[2]]}}));
  result.minimum_core_outer_clearance=std::numeric_limits<double>::infinity();
  for(std::size_t core_index=0U;core_index<input.retained_core_tetrahedra.size();++core_index) {
    const auto& cell=input.retained_core_tetrahedra[core_index];
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      std::array<Vec3,3> core_face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)if(corner!=omitted)
        core_face[cursor++]=input.vertices[cell[corner]];
      for(std::size_t outer_index=0U;outer_index<input.outer_faces.size();++outer_index) {
        const auto& outer=input.outer_faces[outer_index];
        const std::array<Vec3,3> outer_face{{input.vertices[outer[0]],
                                              input.vertices[outer[1]],
                                              input.vertices[outer[2]]}};
        const auto outer_bounds_index=outer_index;
        const auto lower_bound_squared=bounds_distance_squared(
            triangle_bounds(core_face),outer_bounds[outer_bounds_index]);
        // The clearance contract only rejects touching or intersecting
        // surfaces.  Axis-aligned separation beyond that tolerance proves an
        // exact triangle-distance calculation cannot change acceptance.
        if(lower_bound_squared>required_clearance*required_clearance) {
          result.minimum_core_outer_clearance=std::min(
              result.minimum_core_outer_clearance,std::sqrt(lower_bound_squared));
          continue;
        }
        const auto clearance=std::sqrt(triangle_distance_squared(core_face,outer_face));
        result.minimum_core_outer_clearance=
            std::min(result.minimum_core_outer_clearance,clearance);
        if(clearance<=required_clearance) {
          result.related_element=core_index;
          return fail(SurfaceCoreInputFailure::core_touches_or_intersects_outer,
                      outer_index);
        }
      }
    }
  }
  result.accepted=true;
  return result;
}

SurfaceCoreTransitionValidation validate_surface_core_transition_output(
    const SurfaceCoreTransitionInput& input, const SurfaceCoreTransitionOutput& output) {
  SurfaceCoreTransitionValidation result;
  result.positive_tetrahedra=true;
  result.unique_tetrahedra=true;
  result.no_strict_tetrahedron_overlap=true;
  result.closed_two_manifold=true;
  result.consistently_oriented_shared_faces=true;
  result.frozen_outer_faces_preserved=true;
  result.retained_core_preserved=true;
  if (!validate_surface_core_transition_input(input).accepted) { result.failure=SurfaceCoreOutputFailure::rejected_input_contract; return result; }
  if ((!output.owned_vertex_ids.empty()&&output.owned_vertex_ids.size()!=output.owned_vertices.size()) ||
      output.owned_vertices.size()>input.maximum_vertices-input.vertices.size()) { result.failure=SurfaceCoreOutputFailure::invalid_owned_vertex_id; return result; }
  std::vector<Vec3> vertices=input.vertices; vertices.insert(vertices.end(),output.owned_vertices.begin(),output.owned_vertices.end());
  std::set<std::uint64_t> ids;
  for(std::size_t i=0;i<input.vertices.size();++i) ids.insert(input.stable_vertex_ids.empty()?static_cast<std::uint64_t>(i):input.stable_vertex_ids[i]);
  for(std::size_t i=0;i<output.owned_vertices.size();++i) {
    if(!finite(output.owned_vertices[i])) {result.failure=SurfaceCoreOutputFailure::invalid_owned_vertex;return result;}
    const auto id=output.owned_vertex_ids.empty()?static_cast<std::uint64_t>(input.vertices.size()+i):output.owned_vertex_ids[i];
    if(!ids.insert(id).second) {result.failure=SurfaceCoreOutputFailure::invalid_owned_vertex_id;return result;}
  }
  std::set<Tet> unique_tets;
  std::map<Face,std::size_t> face_uses;
  std::map<Face,std::vector<int>> face_sides;
  for (const auto cell:output.tetrahedra) {
    bool valid_indices=true;
    for (const auto id:cell) valid_indices=valid_indices && id<vertices.size();
    const auto key=tet(cell);
    bool geometrically_degenerate=!valid_indices;
    if(valid_indices) {
      std::array<std::uint64_t,4> stable{};
      for(std::size_t corner=0U;corner<4U;++corner) {
        const auto vertex=cell[corner];
        stable[corner]=vertex<input.vertices.size()
            ?(input.stable_vertex_ids.empty()?static_cast<std::uint64_t>(vertex):
              input.stable_vertex_ids[vertex])
            :(output.owned_vertex_ids.empty()?static_cast<std::uint64_t>(vertex):
              output.owned_vertex_ids[vertex-input.vertices.size()]);
      }
      // Geometric validity is determined by the exact orientation of the
      // emitted binary64 positions. Exact-plane provenance describes source
      // construction; it must not turn a positive Wang tetrahedron into an
      // invalid cell through an additional, non-Wang acceptance rule.
      geometrically_degenerate=
          exact_orientation_3d(vertices[cell[0]],vertices[cell[1]],
                               vertices[cell[2]],vertices[cell[3]])==
              ExactPredicateSign::zero;
    }
    if (!valid_indices || key[0]==key[1] || key[1]==key[2] || key[2]==key[3] ||
        geometrically_degenerate) {
      result.positive_tetrahedra=false; ++result.degenerate_tetrahedra; continue;
    }
    if (!unique_tets.insert(key).second) { result.unique_tetrahedra=false; ++result.duplicate_tetrahedra; }
    for (std::size_t opposite=0; opposite<4U; ++opposite) {
      Face local{}; std::size_t cursor{};
      for (std::size_t vertex=0; vertex<4U; ++vertex) if (vertex!=opposite) local[cursor++]=cell[vertex];
      const auto canonical=face(local); ++face_uses[canonical];
      const auto side=exact_orientation_3d(
          vertices[canonical[0]],vertices[canonical[1]],vertices[canonical[2]],
          vertices[cell[opposite]]);
      face_sides[canonical].push_back(static_cast<int>(side));
    }
  }
  // A retained explicit tetrahedral core cannot conform to a refined core
  // interface: its original literal face would remain.  Making that a
  // specific rejection prevents metadata from falsely claiming conformity.
  for(const auto& parent:input.core_parent_facets) if(parent.mode==FacetPreservationMode::geometric) {
    result.failure=SurfaceCoreOutputFailure::unsupported_geometric_core; return result;
  }
  std::map<std::uint64_t,std::uint32_t> input_index_by_stable_id;
  for(std::size_t i=0;i<input.vertices.size();++i)
    input_index_by_stable_id.emplace(input.stable_vertex_ids.empty()?static_cast<std::uint64_t>(i):input.stable_vertex_ids[i],static_cast<std::uint32_t>(i));
  const auto parent_vertices=[&](FrozenFacetIdentity id) {
    std::sort(id.vertex_ids.begin(),id.vertex_ids.end()); std::array<FrozenFacetVertex,3> r{};
    for(std::size_t i=0;i<3U;++i) { const auto found=input_index_by_stable_id.find(id.vertex_ids[i]); if(found==input_index_by_stable_id.end()) return std::array<FrozenFacetVertex,3>{}; r[i]={id.vertex_ids[i],input.vertices[found->second]}; }
    return r;
  };
  std::set<Face> outer;
  for(std::size_t i=0;i<input.outer_faces.size();++i) {
    const auto triangle=input.outer_faces[i];
    FrozenFacetIdentity parent{{
      input.stable_vertex_ids.empty()?triangle[0]:input.stable_vertex_ids[triangle[0]],
      input.stable_vertex_ids.empty()?triangle[1]:input.stable_vertex_ids[triangle[1]],
      input.stable_vertex_ids.empty()?triangle[2]:input.stable_vertex_ids[triangle[2]]}};
    std::sort(parent.vertex_ids.begin(),parent.vertex_ids.end());
    const auto mode=input.outer_parent_facets.empty()?FacetPreservationMode::literal:input.outer_parent_facets[i].mode;
    if(mode==FacetPreservationMode::literal) {
      const auto fixed=face(triangle); outer.insert(fixed);
      const auto use=face_uses.find(fixed);
      if(use==face_uses.end()||use->second!=1U) { result.frozen_outer_faces_preserved=false;++result.missing_outer_faces;result.failure=SurfaceCoreOutputFailure::literal_facet_changed; }
      continue;
    }
    FrozenFacetSplit split; split.parent=parent; split.mode=mode; bool found{};
    for(const auto& reported:output.outer_preserved_facets) {
      auto id=reported.exact.parent;std::sort(id.vertex_ids.begin(),id.vertex_ids.end()); if(id!=parent) continue;
      if(!reported.exact.emitted_by_local_chunk) {result.failure=SurfaceCoreOutputFailure::geometric_facet_not_emitted;return result;}
      if(!found) {split.owner_chunk=reported.exact.owner_chunk;found=true;}
      split.subfaces.push_back(reported.exact);
      for(std::size_t corner=0;corner<3U;++corner) {
        const auto index=reported.vertices[corner];
        if(index>=vertices.size()) {result.failure=SurfaceCoreOutputFailure::invalid_output_index;return result;}
        const auto expected=evaluate_facet_barycentric(parent_vertices(parent),reported.exact.corners[corner]);
        if(length(vertices[index]-expected)>input.coordinate_scale*1e-11) {result.failure=SurfaceCoreOutputFailure::invalid_facet_preservation;++result.invalid_preserved_subfaces;return result;}
      }
      const auto actual=face(reported.vertices); outer.insert(actual);
      const auto use=face_uses.find(actual);
      if(use==face_uses.end()||use->second!=1U) {result.failure=SurfaceCoreOutputFailure::geometric_facet_not_emitted;return result;}
    }
    if(!found) {result.failure=SurfaceCoreOutputFailure::missing_facet_preservation;++result.missing_outer_parent_facets;return result;}
    if(!validate_frozen_facet_split(split)) {result.failure=SurfaceCoreOutputFailure::invalid_facet_preservation;++result.invalid_preserved_subfaces;return result;}
  }
  for (const auto& [boundary,count]:face_uses) {
    if (count>2U) { result.closed_two_manifold=false; ++result.nonmanifold_faces; }
    if(count==2U&&face_sides[boundary][0]==face_sides[boundary][1]) {result.consistently_oriented_shared_faces=false;++result.same_sided_shared_faces;}
    if (count==1U && !outer.contains(boundary)) {
      result.closed_two_manifold=false; ++result.unexpected_boundary_faces;
    }
  }
  for (const auto core:input.retained_core_tetrahedra)
    if (!unique_tets.contains(tet(core))) {
      result.retained_core_preserved=false; ++result.missing_core_tetrahedra;
    }
  struct OverlapCandidate {
    std::size_t tetrahedron{};
    std::array<Vec3,4> points{};
    TetrahedronBounds bounds{};
  };
  std::vector<OverlapCandidate> overlap_candidates;
  overlap_candidates.reserve(output.tetrahedra.size());
  for(std::size_t index=0U;index<output.tetrahedra.size();++index) {
    OverlapCandidate candidate;candidate.tetrahedron=index;
    bool valid_indices=true;
    for(std::size_t corner=0U;corner<4U;++corner) {
      const auto vertex=output.tetrahedra[index][corner];
      valid_indices=valid_indices&&vertex<vertices.size();
      if(valid_indices)candidate.points[corner]=vertices[vertex];
    }
    if(!valid_indices)continue;
    candidate.bounds=tetrahedron_bounds(candidate.points);
    overlap_candidates.push_back(candidate);
  }
  std::sort(overlap_candidates.begin(),overlap_candidates.end(),
            [](const auto& first,const auto& second) {
              return first.bounds.minimum.x<second.bounds.minimum.x;
            });
  // This is only a broad phase.  A strict overlap requires overlapping
  // axis-aligned bounds, so no possible pair is discarded; every survivor is
  // still tested by the established separating-axis predicate above.
  for(std::size_t left=0U;left<overlap_candidates.size();++left) {
    const auto& first=overlap_candidates[left];
    for(std::size_t right=left+1U;right<overlap_candidates.size()&&
        overlap_candidates[right].bounds.minimum.x<first.bounds.maximum.x;++right) {
      const auto& second=overlap_candidates[right];
      if(!bounds_overlap(first.bounds,second.bounds))continue;
      if(strict_tetrahedra_overlap_impl(first.points,second.points)) {
        result.no_strict_tetrahedron_overlap=false;
        ++result.tetrahedron_overlap_pairs;
      }
    }
  }
  // Preservation failures are reported at their point of discovery.  The
  // aggregate geometry checks deliberately continue to collect all counters,
  // so assign the first deterministic geometry failure only after that audit.
  // A caller can now distinguish a rejected output from an output that was
  // never assembled, without treating a false `valid` flag as unexplained.
  if(result.failure==SurfaceCoreOutputFailure::none) {
    if(!result.positive_tetrahedra)
      result.failure=SurfaceCoreOutputFailure::non_positive_tetrahedra;
    else if(!result.unique_tetrahedra)
      result.failure=SurfaceCoreOutputFailure::duplicate_tetrahedra;
    else if(!result.no_strict_tetrahedron_overlap)
      result.failure=SurfaceCoreOutputFailure::tetrahedron_overlap;
    else if(!result.closed_two_manifold)
      result.failure=SurfaceCoreOutputFailure::nonmanifold_output;
    else if(!result.consistently_oriented_shared_faces)
      result.failure=SurfaceCoreOutputFailure::inconsistent_shared_face;
    else if(!result.retained_core_preserved)
      result.failure=SurfaceCoreOutputFailure::missing_retained_core;
  }
  result.valid=result.positive_tetrahedra && result.unique_tetrahedra && result.no_strict_tetrahedron_overlap && result.closed_two_manifold && result.consistently_oriented_shared_faces &&
      result.frozen_outer_faces_preserved && result.retained_core_preserved && result.failure==SurfaceCoreOutputFailure::none;
  return result;
}

bool strict_tetrahedra_overlap(
    const std::array<Vec3,4>& first,const std::array<Vec3,4>& second) {
  return strict_tetrahedra_overlap_impl(first,second);
}

SurfaceCoreConstructionResult construct_homologous_surface_core_transition(
    const SurfaceCoreTransitionInput& input, const SurfaceCoreConstructionOptions& options) {
  SurfaceCoreConstructionResult result;
  if (!validate_surface_core_transition_input(input).accepted) return result;
  if (options.outer_to_core_vertex.size()!=input.vertices.size()) {
    result.failure=SurfaceCoreConstructionFailure::missing_or_invalid_correspondence; return result;
  }
  std::set<std::uint32_t> mapped_vertices;
  for (const auto vertex:options.outer_to_core_vertex)
    if (vertex>=input.vertices.size() || !mapped_vertices.insert(vertex).second) {
      result.failure=SurfaceCoreConstructionFailure::missing_or_invalid_correspondence; return result;
    }
  std::map<Face,std::size_t> core_faces;
  for (const auto cell:input.retained_core_tetrahedra)
    for (std::size_t opposite=0; opposite<4U; ++opposite) {
      Face local{}; std::size_t cursor{};
      for (std::size_t vertex=0; vertex<4U; ++vertex) if (vertex!=opposite) local[cursor++]=cell[vertex];
      ++core_faces[face(local)];
    }
  std::set<Face> expected_core_front;
  for (const auto outer:input.outer_faces) {
    Face inner{};
    for (std::size_t i=0; i<3U; ++i) inner[i]=options.outer_to_core_vertex[outer[i]];
    expected_core_front.insert(face(inner));
  }
  std::set<Face> actual_core_front;
  for (const auto& [core_face,uses]:core_faces) if (uses==1U) actual_core_front.insert(core_face);
  if (actual_core_front!=expected_core_front) {
    result.failure=SurfaceCoreConstructionFailure::core_front_not_homologous; return result;
  }
  for (const auto outer_face:input.outer_faces) {
    std::array<std::uint32_t,3> outer=outer_face, inner{};
    for (std::size_t i=0; i<3U; ++i) inner[i]=options.outer_to_core_vertex[outer[i]];
    // Sorting by frozen outer identity gives one globally deterministic split
    // for every shared prism side quad; no fixture or geometry heuristic is
    // involved in this initial topology.
    std::array<std::size_t,3> order{{0U,1U,2U}};
    std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) { return outer[a]<outer[b]; });
    std::array<std::uint32_t,3> sorted_outer{}, sorted_inner{};
    for (std::size_t i=0; i<3U; ++i) { sorted_outer[i]=outer[order[i]]; sorted_inner[i]=inner[order[i]]; }
    result.output.tetrahedra.push_back({{sorted_outer[0],sorted_outer[1],sorted_outer[2],sorted_inner[0]}});
    result.output.tetrahedra.push_back({{sorted_outer[1],sorted_outer[2],sorted_inner[0],sorted_inner[1]}});
    result.output.tetrahedra.push_back({{sorted_outer[2],sorted_inner[0],sorted_inner[1],sorted_inner[2]}});
  }
  result.output.tetrahedra.insert(result.output.tetrahedra.end(),
      input.retained_core_tetrahedra.begin(),input.retained_core_tetrahedra.end());
  result.validation=validate_surface_core_transition_output(input,result.output);
  if (!result.validation.valid) {
    result.output.tetrahedra.clear();
    result.failure=SurfaceCoreConstructionFailure::geometry_gate_rejected; return result;
  }
  result.minimum_dihedral_degrees=180.0;
  for (const auto cell:result.output.tetrahedra) {
    std::array<Vec3,4> points{};
    for (std::size_t i=0; i<4U; ++i) points[i]=input.vertices[cell[i]];
    for (std::size_t first=0; first<4U; ++first) for (std::size_t second=first+1U; second<4U; ++second) {
      std::array<Vec3,3> first_face{},second_face{}; std::size_t first_cursor{},second_cursor{};
      for (std::size_t i=0; i<4U; ++i) { if (i!=first) first_face[first_cursor++]=points[i]; if (i!=second) second_face[second_cursor++]=points[i]; }
      Vec3 first_normal=cross(first_face[1]-first_face[0],first_face[2]-first_face[0]);
      Vec3 second_normal=cross(second_face[1]-second_face[0],second_face[2]-second_face[0]);
      if (dot(first_normal,points[first]-first_face[0])>0.0) first_normal=first_normal*-1.0;
      if (dot(second_normal,points[second]-second_face[0])>0.0) second_normal=second_normal*-1.0;
      const double cosine=std::clamp(dot(first_normal,second_normal)/(length(first_normal)*length(second_normal)),-1.0,1.0);
      const double degrees=(std::numbers::pi-std::acos(cosine))*180.0/std::numbers::pi;
      result.minimum_dihedral_degrees=std::min(result.minimum_dihedral_degrees,degrees);
      result.maximum_dihedral_degrees=std::max(result.maximum_dihedral_degrees,degrees);
    }
  }
  if (result.minimum_dihedral_degrees<options.minimum_dihedral_degrees ||
      result.maximum_dihedral_degrees>options.maximum_dihedral_degrees) {
    result.output.tetrahedra.clear();
    result.failure=SurfaceCoreConstructionFailure::quality_gate_rejected; return result;
  }
  result.failure=SurfaceCoreConstructionFailure::none;
  result.succeeded=true;
  return result;
}

}  // namespace tetra::probes
