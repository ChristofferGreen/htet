#include "tetra_viewer/viewer_scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numeric>
#include <span>
#include <stdexcept>

namespace tetra_viewer {

TetrahedronQuality evaluate_tetrahedron_quality(
    const std::array<tetra::Vec3,4>& points) {
  const auto dot=[](tetra::Vec3 first,tetra::Vec3 second){
    return first.x*second.x+first.y*second.y+first.z*second.z;
  };
  const auto cross=[](tetra::Vec3 first,tetra::Vec3 second){
    return tetra::Vec3{first.y*second.z-first.z*second.y,
                       first.z*second.x-first.x*second.z,
                       first.x*second.y-first.y*second.x};
  };
  const auto ab=points[1]-points[0],ac=points[2]-points[0],ad=points[3]-points[0];
  TetrahedronQuality result;
  result.signed_six_volume=dot(ab,cross(ac,ad));
  if(result.signed_six_volume<=0.0)return result;

  double edge_squared_sum{},maximum_edge_squared{};
  for(std::size_t first=0;first<4;++first){
    for(std::size_t second=first+1;second<4;++second){
      const auto edge=points[second]-points[first];
      const double squared=dot(edge,edge);
      edge_squared_sum+=squared;
      maximum_edge_squared=std::max(maximum_edge_squared,squared);
    }
  }
  result.mean_ratio=edge_squared_sum>0.0
      ?12.0*std::pow(result.signed_six_volume*0.5,2.0/3.0)/edge_squared_sum:0.0;

  constexpr std::array<std::array<std::size_t,3>,4> opposite_faces{{
      {{1,2,3}},{{0,2,3}},{{0,1,3}},{{0,1,2}}}};
  std::array<tetra::Vec3,4> outward_normals{};
  double surface_area{};
  for(std::size_t opposite=0;opposite<4;++opposite){
    const auto face=opposite_faces[opposite];
    const auto origin=points[face[0]];
    auto normal=cross(points[face[1]]-origin,points[face[2]]-origin);
    if(dot(normal,points[opposite]-origin)>0.0)normal=normal*-1.0;
    outward_normals[opposite]=normal;
    surface_area+=0.5*std::sqrt(dot(normal,normal));
  }
  if(surface_area>0.0&&maximum_edge_squared>0.0){
    const double volume=result.signed_six_volume/6.0;
    result.volume_surface_longest_edge=
        6.0*std::sqrt(6.0)*volume/(surface_area*std::sqrt(maximum_edge_squared));
  }

  constexpr double radians_to_degrees=57.295779513082320876;
  result.minimum_dihedral_sine=1.0;
  result.minimum_dihedral_degrees=180.0;
  for(std::size_t first=0;first<4;++first){
    for(std::size_t second=first+1;second<4;++second){
      const double denominator=std::sqrt(dot(outward_normals[first],outward_normals[first])*
                                         dot(outward_normals[second],outward_normals[second]));
      if(denominator<=1.0e-30){
        result.minimum_dihedral_sine=0.0;
        result.minimum_dihedral_degrees=0.0;
        result.maximum_dihedral_degrees=180.0;
        continue;
      }
      const double cosine=std::clamp(-dot(outward_normals[first],outward_normals[second])/
                                     denominator,-1.0,1.0);
      const double radians=std::acos(cosine);
      const double degrees=radians*radians_to_degrees;
      result.minimum_dihedral_sine=std::min(result.minimum_dihedral_sine,std::sin(radians));
      result.minimum_dihedral_degrees=std::min(result.minimum_dihedral_degrees,degrees);
      result.maximum_dihedral_degrees=std::max(result.maximum_dihedral_degrees,degrees);
    }
  }
  return result;
}

ConnectedComplexValidation validate_connected_complex(
    const PreparedScene& scene,const tetra::TetMesh* hierarchy) {
  ConnectedComplexValidation result;
  result.regions_aligned=scene.connected_volume_regions.size()==
      scene.connected_volume_tetrahedra.size();
  if(scene.connected_volume_tetrahedra.empty())return result;

  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  struct FaceOwner {
    std::array<std::size_t,3> key{};
    std::size_t tetrahedron{};
  };
  std::vector<FaceOwner> owners;
  owners.reserve(scene.connected_volume_tetrahedra.size()*4);
  std::vector<double> maximum_edges(scene.connected_volume_tetrahedra.size());
  result.positive_volumes=true;
  result.minimum_signed_six_volume=std::numeric_limits<double>::infinity();
  for(std::size_t index=0;index<scene.connected_volume_tetrahedra.size();++index){
    const auto& tet=scene.connected_volume_tetrahedra[index];
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)
      points[corner]=scene.connected_volume_vertices[tet[corner]];
    const auto quality=evaluate_tetrahedron_quality(points);
    result.minimum_signed_six_volume=std::min(
        result.minimum_signed_six_volume,quality.signed_six_volume);
    result.positive_volumes=result.positive_volumes&&quality.signed_six_volume>1.0e-15;
    for(std::size_t first=0;first<4;++first)
      for(std::size_t second=first+1;second<4;++second){
        const auto delta=points[second]-points[first];
        maximum_edges[index]=std::max(maximum_edges[index],
            std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z));
      }
    for(const auto face:faces){
      std::array<std::size_t,3> key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      std::sort(key.begin(),key.end());
      owners.push_back({key,index});
    }
  }
  std::sort(owners.begin(),owners.end(),[](const auto& first,const auto& second){
    return first.key<second.key;
  });
  result.manifold_face_incidence=true;
  result.exterior_owned_by_surface=true;
  for(std::size_t begin=0;begin<owners.size();){
    std::size_t end=begin+1;
    while(end<owners.size()&&owners[end].key==owners[begin].key)++end;
    const auto incidence=end-begin;
    if(incidence>2){
      ++result.nonmanifold_faces;
      result.manifold_face_incidence=false;
    }else if(incidence==1){
      ++result.exterior_faces;
      const bool surface=std::ranges::all_of(owners[begin].key,[&](std::size_t vertex){
        return vertex<scene.connected_volume_surface_vertices.size()&&
               scene.connected_volume_surface_vertices[vertex]!=0U;
      });
      if(!surface){
        ++result.unmatched_non_surface_faces;
        result.exterior_owned_by_surface=false;
      }
    }else{
      const double first=maximum_edges[owners[begin].tetrahedron];
      const double second=maximum_edges[owners[begin+1].tetrahedron];
      const double smaller=std::min(first,second);
      if(smaller>1.0e-15){
        const double ratio=std::max(first,second)/smaller;
        if(ratio>result.maximum_adjacent_edge_ratio){
          result.maximum_adjacent_edge_ratio=ratio;
          result.maximum_ratio_first_region=
              scene.connected_volume_regions[owners[begin].tetrahedron];
          result.maximum_ratio_second_region=
              scene.connected_volume_regions[owners[begin+1].tetrahedron];
        }
      }
      if(hierarchy!=nullptr){
        const auto parent_edge=[&](std::size_t tetrahedron){
          const auto parent=scene.connected_volume_parents[tetrahedron];
          const auto& vertices=hierarchy->tetrahedron(parent).vertices;
          double maximum{};
          for(std::size_t a=0;a<4;++a)for(std::size_t b=a+1;b<4;++b){
            const auto delta=hierarchy->vertices()[vertices[b]]-hierarchy->vertices()[vertices[a]];
            maximum=std::max(maximum,
                std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z));
          }
          return maximum;
        };
        const double first_parent=parent_edge(owners[begin].tetrahedron);
        const double second_parent=parent_edge(owners[begin+1].tetrahedron);
        const double smaller_parent=std::min(first_parent,second_parent);
        if(smaller_parent>1.0e-15)
          result.maximum_adjacent_parent_edge_ratio=std::max(
              result.maximum_adjacent_parent_edge_ratio,
              std::max(first_parent,second_parent)/smaller_parent);
        const auto first_depth=hierarchy->refinement_depth(
            scene.connected_volume_parents[owners[begin].tetrahedron]);
        const auto second_depth=hierarchy->refinement_depth(
            scene.connected_volume_parents[owners[begin+1].tetrahedron]);
        result.maximum_adjacent_parent_depth_difference=std::max(
            result.maximum_adjacent_parent_depth_difference,
            first_depth>second_depth?first_depth-second_depth:second_depth-first_depth);
      }
    }
    begin=end;
  }
  result.graded_parent_band=hierarchy==nullptr||
      result.maximum_adjacent_parent_depth_difference<=
          tetra::subdivision_depth_increment(hierarchy->subdivision_method());
  result.valid=result.positive_volumes&&result.manifold_face_incidence&&
      result.exterior_owned_by_surface&&result.regions_aligned&&
      result.graded_parent_band&&result.exterior_faces>0;
  return result;
}

namespace {

using PackedEdge = std::uint64_t;

struct MaterialFace {
  std::array<tetra::VertexId, 3> key{};
  std::array<tetra::VertexId, 3> vertices{};
  tetra::TetId tetrahedron{};
};

struct LayerFace {
  std::array<tetra::VertexId, 3> key{};
  std::array<tetra::VertexId, 3> vertices{};
  std::size_t tetrahedron{};
};

struct ConnectedFace {
  std::array<std::size_t, 3> key{};
  std::array<std::size_t, 3> vertices{};
  std::size_t tetrahedron{};
};

struct CrossedEdge {
  PackedEdge key{};
  std::size_t vertex{};
};

PackedEdge pack_edge(tetra::VertexId first, tetra::VertexId second) {
  if (second < first) std::swap(first, second);
  return (static_cast<PackedEdge>(first) << 32U) | static_cast<PackedEdge>(second);
}

std::array<tetra::VertexId, 2> unpack_edge(PackedEdge edge) {
  return {static_cast<tetra::VertexId>(edge >> 32U), static_cast<tetra::VertexId>(edge)};
}

tetra::Vec3 face_normal(tetra::Vec3 a, tetra::Vec3 b, tetra::Vec3 c) {
  return {(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
          (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
          (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
}

void prepare_surface_render_attributes(PreparedScene& scene) {
  const std::size_t triangle_count=scene.triangle_vertices.size()/3;
  if (triangle_count==0) return;
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto normalize=[&dot](tetra::Vec3 value){
    const double length=std::sqrt(dot(value,value));
    return length>1e-15?value/length:tetra::Vec3{};
  };
  for(std::size_t triangle=0;triangle<triangle_count;++triangle){
    const auto& first=scene.triangle_vertices[triangle*3];
    const auto normal=normalize({first.normal[0],first.normal[1],first.normal[2]});
    for(std::size_t vertex=0;vertex<3;++vertex){
      auto& output=scene.triangle_vertices[triangle*3+vertex];
      // Flat lighting and barycentric wireframes are render inputs, not
      // diagnostics, and must always be prepared.
      output.normal[0]=static_cast<float>(normal.x);
      output.normal[1]=static_cast<float>(normal.y);
      output.normal[2]=static_cast<float>(normal.z);
      output.barycentric[0]=vertex==0?1.0F:0.0F;
      output.barycentric[1]=vertex==1?1.0F:0.0F;
      output.barycentric[2]=vertex==2?1.0F:0.0F;
    }
  }
}

void annotate_surface_diagnostics(PreparedScene& scene, const tetra::Sphere& sphere) {
  scene.surface_diagnostics_available=true;
  const std::size_t triangle_count=scene.triangle_vertices.size()/3;
  if (triangle_count==0) return;
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  using PointKey=std::array<long long,3>;
  struct DiagnosticEdge { std::array<PointKey,2> key{}; std::size_t triangle{}; };
  const auto point_key=[](const SceneVertex& vertex){
    constexpr double scale=1.0e10;
    return PointKey{{std::llround(vertex.position[0]*scale),std::llround(vertex.position[1]*scale),std::llround(vertex.position[2]*scale)}};
  };
  std::vector<tetra::Vec3> normals(triangle_count);
  std::vector<float> dihedral_degrees(triangle_count);
  std::vector<float> normal_error_degrees(triangle_count);
  std::vector<DiagnosticEdge> edges;
  edges.reserve(triangle_count*3);
  constexpr double radians_to_degrees=57.295779513082320876;
  for(std::size_t triangle=0;triangle<triangle_count;++triangle){
    const auto& first=scene.triangle_vertices[triangle*3];
    const auto& second=scene.triangle_vertices[triangle*3+1];
    const auto& third=scene.triangle_vertices[triangle*3+2];
    normals[triangle]={first.normal[0],first.normal[1],first.normal[2]};
    const tetra::Vec3 centre{(first.position[0]+second.position[0]+third.position[0])/3.0,
                             (first.position[1]+second.position[1]+third.position[1])/3.0,
                             (first.position[2]+second.position[2]+third.position[2])/3.0};
    const auto analytic=sphere.normal(centre);
    normal_error_degrees[triangle]=static_cast<float>(
        std::acos(std::clamp(std::abs(dot(normals[triangle],analytic)),0.0,1.0))*radians_to_degrees);
    const std::array<tetra::Vec3,3> triangle_points{{
        {first.position[0],first.position[1],first.position[2]},
        {second.position[0],second.position[1],second.position[2]},
        {third.position[0],third.position[1],third.position[2]}}};
    std::array<double,3> edge_lengths{};
    for(std::size_t edge=0;edge<3;++edge){
      const auto delta=triangle_points[(edge+1)%3]-triangle_points[edge];
      edge_lengths[edge]=std::sqrt(dot(delta,delta));
    }
    const auto [minimum_edge,maximum_edge]=std::minmax_element(edge_lengths.begin(),edge_lengths.end());
    if(*minimum_edge>1.0e-15)
      scene.maximum_surface_triangle_edge_ratio=std::max(
          scene.maximum_surface_triangle_edge_ratio,*maximum_edge/ *minimum_edge);
    for(std::size_t corner=0;corner<3;++corner){
      const auto first_edge=triangle_points[(corner+1)%3]-triangle_points[corner];
      const auto second_edge=triangle_points[(corner+2)%3]-triangle_points[corner];
      const double denominator=std::sqrt(dot(first_edge,first_edge)*dot(second_edge,second_edge));
      if(denominator>1.0e-30)
        scene.minimum_surface_triangle_angle_degrees=std::min(
            scene.minimum_surface_triangle_angle_degrees,
            std::acos(std::clamp(dot(first_edge,second_edge)/denominator,-1.0,1.0))*radians_to_degrees);
    }
    const std::array<PointKey,3> points{{point_key(first),point_key(second),point_key(third)}};
    for(const auto pair:std::array<std::array<std::size_t,2>,3>{{{{0,1}},{{1,2}},{{2,0}}}}){
      std::array<PointKey,2> key{{points[pair[0]],points[pair[1]]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      edges.push_back({key,triangle});
    }
  }
  std::sort(edges.begin(),edges.end(),[](const DiagnosticEdge& first,const DiagnosticEdge& second){
    return first.key<second.key;
  });
  for(std::size_t begin=0;begin<edges.size();){
    std::size_t end=begin+1;
    while(end<edges.size()&&edges[end].key==edges[begin].key)++end;
    if(end-begin==2){
      const auto first=edges[begin].triangle,second=edges[begin+1].triangle;
      const float angle=static_cast<float>(std::acos(std::clamp(dot(normals[first],normals[second]),-1.0,1.0))*radians_to_degrees);
      dihedral_degrees[first]=std::max(dihedral_degrees[first],angle);
      dihedral_degrees[second]=std::max(dihedral_degrees[second],angle);
    }else{
      // Open or non-manifold topology is itself a worst-case discontinuity.
      for(std::size_t edge=begin;edge<end;++edge)dihedral_degrees[edges[edge].triangle]=180.0F;
    }
    begin=end;
  }
  for(std::size_t triangle=0;triangle<triangle_count;++triangle){
    for(std::size_t vertex=0;vertex<3;++vertex){
      auto& output=scene.triangle_vertices[triangle*3+vertex];
      output.diagnostics[0]=dihedral_degrees[triangle];
      output.diagnostics[1]=normal_error_degrees[triangle];
    }
    scene.mean_dihedral_degrees+=dihedral_degrees[triangle];
    scene.maximum_dihedral_degrees=std::max(scene.maximum_dihedral_degrees,static_cast<double>(dihedral_degrees[triangle]));
    scene.mean_normal_error_degrees+=normal_error_degrees[triangle];
    scene.maximum_normal_error_degrees=std::max(scene.maximum_normal_error_degrees,static_cast<double>(normal_error_degrees[triangle]));
  }
  scene.mean_dihedral_degrees/=static_cast<double>(triangle_count);
  scene.mean_normal_error_degrees/=static_cast<double>(triangle_count);
  const auto percentile=[](std::vector<float> values,double fraction){
    std::sort(values.begin(),values.end());
    const auto index=static_cast<std::size_t>(std::ceil(fraction*static_cast<double>(values.size())))-1;
    return static_cast<double>(values[std::min(index,values.size()-1)]);
  };
  scene.percentile95_dihedral_degrees=percentile(dihedral_degrees,0.95);
  scene.percentile99_dihedral_degrees=percentile(dihedral_degrees,0.99);
  scene.percentile95_normal_error_degrees=percentile(normal_error_degrees,0.95);
  scene.percentile99_normal_error_degrees=percentile(normal_error_degrees,0.99);
}

double signed_six_volume(tetra::Vec3 a, tetra::Vec3 b, tetra::Vec3 c, tetra::Vec3 d) {
  const auto ab = b-a, ac = c-a, ad = d-a;
  return ab.x*(ac.y*ad.z-ac.z*ad.y)-ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
}

template <typename AppendTriangle>
void triangulate_polygon(std::span<const std::size_t> polygon, AppendTriangle&& append_triangle) {
  if (polygon.size() == 3) {
    append_triangle(std::array<std::size_t, 3>{{polygon[0], polygon[1], polygon[2]}});
    return;
  }
  if (polygon.size() != 4) return;
  auto first_diagonal = std::array<std::size_t, 2>{{polygon[0], polygon[2]}};
  auto second_diagonal = std::array<std::size_t, 2>{{polygon[1], polygon[3]}};
  std::sort(first_diagonal.begin(), first_diagonal.end());
  std::sort(second_diagonal.begin(), second_diagonal.end());
  if (first_diagonal < second_diagonal) {
    append_triangle(std::array<std::size_t, 3>{{polygon[0], polygon[1], polygon[2]}});
    append_triangle(std::array<std::size_t, 3>{{polygon[0], polygon[2], polygon[3]}});
  } else {
    append_triangle(std::array<std::size_t, 3>{{polygon[0], polygon[1], polygon[3]}});
    append_triangle(std::array<std::size_t, 3>{{polygon[1], polygon[2], polygon[3]}});
  }
}

// The six tetrahedralizations of a triangular prism. The three quad-face
// diagonals identify which templates can be substituted without changing a
// shared hierarchy face. In the first atlas stage we deliberately preserve
// the two hierarchy-face diagonals emitted by the old template; candidates 0
// and 1 then expose the two legal surface diagonals of the two-inside case.
inline constexpr std::array prism_templates{
    std::array{std::array<std::size_t,4>{0,1,2,3},std::array<std::size_t,4>{1,2,3,4},
               std::array<std::size_t,4>{2,3,4,5}},
    std::array{std::array<std::size_t,4>{0,1,2,3},std::array<std::size_t,4>{1,2,3,5},
               std::array<std::size_t,4>{1,3,4,5}},
    std::array{std::array<std::size_t,4>{0,1,2,4},std::array<std::size_t,4>{0,2,3,4},
               std::array<std::size_t,4>{2,3,4,5}},
    std::array{std::array<std::size_t,4>{0,1,2,4},std::array<std::size_t,4>{0,2,4,5},
               std::array<std::size_t,4>{0,3,4,5}},
    std::array{std::array<std::size_t,4>{0,1,2,5},std::array<std::size_t,4>{0,1,3,5},
               std::array<std::size_t,4>{1,3,4,5}},
    std::array{std::array<std::size_t,4>{0,1,2,5},std::array<std::size_t,4>{0,1,4,5},
               std::array<std::size_t,4>{0,3,4,5}},
};

inline constexpr std::array prism_quad_diagonals{
    std::array{std::array<std::size_t,2>{1,3},std::array<std::size_t,2>{2,3},
               std::array<std::size_t,2>{2,4}},
    std::array{std::array<std::size_t,2>{1,3},std::array<std::size_t,2>{2,3},
               std::array<std::size_t,2>{1,5}},
    std::array{std::array<std::size_t,2>{0,4},std::array<std::size_t,2>{2,3},
               std::array<std::size_t,2>{2,4}},
    std::array{std::array<std::size_t,2>{0,4},std::array<std::size_t,2>{0,5},
               std::array<std::size_t,2>{2,4}},
    std::array{std::array<std::size_t,2>{1,3},std::array<std::size_t,2>{0,5},
               std::array<std::size_t,2>{1,5}},
    std::array{std::array<std::size_t,2>{0,4},std::array<std::size_t,2>{0,5},
               std::array<std::size_t,2>{1,5}},
};

double triangle_angle_energy(const std::array<tetra::Vec3,3>& points) {
  const auto dot=[](tetra::Vec3 first,tetra::Vec3 second){
    return first.x*second.x+first.y*second.y+first.z*second.z;
  };
  constexpr double target=1.0471975511965977462;
  double energy{};
  for(std::size_t corner=0;corner<3;++corner){
    const auto first=points[(corner+1)%3]-points[corner];
    const auto second=points[(corner+2)%3]-points[corner];
    const double denominator=std::sqrt(dot(first,first)*dot(second,second));
    if(denominator<=1.0e-30)return std::numeric_limits<double>::infinity();
    const double difference=std::acos(std::clamp(dot(first,second)/denominator,-1.0,1.0))-target;
    energy+=difference*difference;
  }
  return energy;
}

struct PrismCandidateScore {
  double surface_energy{};
  double minimum_mean_ratio{1.0};
  double minimum_volume_surface_quality{1.0};
  double minimum_dihedral_sine{1.0};
  double maximum_dihedral_degrees{};
  std::size_t template_index{};
};

bool better_prism_candidate(const PrismCandidateScore& candidate,
                            const PrismCandidateScore& best,
                            StencilSelectionObjective objective) {
  constexpr double epsilon=1.0e-14;
  const auto less=[&](double first,double second){return first<second-epsilon;};
  const auto greater=[&](double first,double second){return first>second+epsilon;};
  if(objective==StencilSelectionObjective::surface){
    if(less(candidate.surface_energy,best.surface_energy))return true;
    if(less(best.surface_energy,candidate.surface_energy))return false;
  }else if(objective==StencilSelectionObjective::balanced){
    const auto combined=[](const PrismCandidateScore& score){
      const double surface_quality=1.0/(1.0+score.surface_energy);
      const double volume_quality=std::sqrt(std::max(
          0.0,score.minimum_volume_surface_quality*score.minimum_dihedral_sine));
      return surface_quality*volume_quality;
    };
    const double candidate_combined=combined(candidate),best_combined=combined(best);
    if(greater(candidate_combined,best_combined))return true;
    if(greater(best_combined,candidate_combined))return false;
  }
  if(greater(candidate.minimum_volume_surface_quality,best.minimum_volume_surface_quality))return true;
  if(greater(best.minimum_volume_surface_quality,candidate.minimum_volume_surface_quality))return false;
  if(greater(candidate.minimum_dihedral_sine,best.minimum_dihedral_sine))return true;
  if(greater(best.minimum_dihedral_sine,candidate.minimum_dihedral_sine))return false;
  if(greater(candidate.minimum_mean_ratio,best.minimum_mean_ratio))return true;
  if(greater(best.minimum_mean_ratio,candidate.minimum_mean_ratio))return false;
  if(less(candidate.maximum_dihedral_degrees,best.maximum_dihedral_degrees))return true;
  if(less(best.maximum_dihedral_degrees,candidate.maximum_dihedral_degrees))return false;
  if(less(candidate.surface_energy,best.surface_energy))return true;
  if(less(best.surface_energy,candidate.surface_energy))return false;
  return candidate.template_index<best.template_index;
}

// Two-material generalized cleaving stencil. Each clipped convex cell is
// decomposed by triangulating its clipped faces consistently and coning them
// to one cell point. Shared hierarchy faces therefore receive exactly the
// same triangulation from both incident cells, independent of local winding.
void build_adaptive_cleaved_volume(PreparedScene& scene, const tetra::TetMesh& mesh,
                                   const tetra::Sphere& sphere,
                                   VolumeConnectionMethod method,
                                   StencilConstruction stencil_construction,
                                   StencilSelectionObjective stencil_selection_objective) {
  const bool quality_stencils=method!=VolumeConnectionMethod::coned_prototype;
  constexpr std::array<std::array<std::size_t, 3>, 4> faces{{
      {{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
  constexpr std::array<std::array<std::size_t, 2>, 6> edges{{
      {{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};
  const auto& source_vertices = mesh.vertices();
  std::vector<double> source_distances(source_vertices.size());
  const bool accelerated_field=sphere.kind==tetra::ImplicitShapeKind::sphere||
      sphere.kind==tetra::ImplicitShapeKind::merging_spheres||
      sphere.kind==tetra::ImplicitShapeKind::cube||
      sphere.kind==tetra::ImplicitShapeKind::capped_cylinder||
      sphere.kind==tetra::ImplicitShapeKind::torus||
      sphere.kind==tetra::ImplicitShapeKind::rounded_cube;
  if(accelerated_field)
    tetra::evaluate_signed_distances(sphere,source_vertices,source_distances);
  else
    for(std::size_t vertex=0;vertex<source_vertices.size();++vertex)
      source_distances[vertex]=sphere.signed_distance(source_vertices[vertex]);
  scene.connected_volume_vertices = source_vertices;
  scene.connected_volume_vertex_kinds.assign(source_vertices.size(),ConnectedVertexKind::hierarchy);
  scene.connected_volume_source_edges.assign(
      source_vertices.size(),{{std::numeric_limits<tetra::VertexId>::max(),
                              std::numeric_limits<tetra::VertexId>::max()}});
  scene.connected_volume_surface_vertices.assign(source_vertices.size(),0U);

  // Bronson et al.'s alpha rule bounds a warp by the shortest altitude of
  // every incident tetrahedron. Use a deliberately conservative fraction;
  // the derived connected layer may move, while the hierarchy itself stays
  // immutable.
  std::vector<double> safe_warp_radius(source_vertices.size(),0.0);
  if(method==VolumeConnectionMethod::adaptive_cleaving){
    std::fill(safe_warp_radius.begin(),safe_warp_radius.end(),
              std::numeric_limits<double>::infinity());
    for(const auto id:mesh.conforming_volume().addresses()){
      const auto& tet=mesh.tetrahedron(id).vertices;
      const std::array<tetra::Vec3,4> points{{source_vertices[tet[0]],source_vertices[tet[1]],
                                              source_vertices[tet[2]],source_vertices[tet[3]]}};
      const double determinant=std::abs(signed_six_volume(points[0],points[1],points[2],points[3]));
      for(std::size_t corner=0;corner<4;++corner){
        std::array<std::size_t,3> opposite{};
        std::size_t cursor{};
        for(std::size_t other=0;other<4;++other)if(other!=corner)opposite[cursor++]=other;
        const auto normal=face_normal(points[opposite[0]],points[opposite[1]],points[opposite[2]]);
        const double double_area=std::sqrt(normal.x*normal.x+normal.y*normal.y+normal.z*normal.z);
        if(double_area>1.0e-30)
          safe_warp_radius[tet[corner]]=std::min(
              safe_warp_radius[tet[corner]],0.10*determinant/double_area);
      }
    }
  }

  std::vector<PackedEdge> crossed_keys;
  crossed_keys.reserve(mesh.conforming_volume().size()*2);
  for (const auto id : mesh.conforming_volume().addresses()) {
    const auto& tet = mesh.tetrahedron(id).vertices;
    std::array<bool, 4> inside{};
    for (std::size_t corner = 0; corner < 4; ++corner)
      inside[corner] = source_distances[tet[corner]] <= 0.0;
    for (const auto edge : edges) if (inside[edge[0]] != inside[edge[1]])
      crossed_keys.push_back(pack_edge(tet[edge[0]], tet[edge[1]]));
  }
  std::sort(crossed_keys.begin(), crossed_keys.end());
  crossed_keys.erase(std::unique(crossed_keys.begin(), crossed_keys.end()), crossed_keys.end());

  std::vector<CrossedEdge> crossings;
  crossings.reserve(crossed_keys.size());
  scene.connected_volume_vertices.reserve(
      source_vertices.size()+crossed_keys.size()+mesh.conforming_volume().size());
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto project_to_surface=[&](tetra::Vec3 point){return sphere.project_to_surface(point);};
  for (const auto key : crossed_keys) {
    const auto endpoints = unpack_edge(key);
    const auto first = source_vertices[endpoints[0]], second = source_vertices[endpoints[1]];
    const auto intersection=sphere.edge_intersection(first,second);
    const auto direction=second-first;
    const double direction_squared=dot(direction,direction);
    const double fraction=direction_squared>1.0e-30?
        dot(intersection-first,direction)/direction_squared:0.0;
    std::size_t vertex{};
    constexpr double snap_epsilon=1.0e-10;
    const auto warpable=[&](tetra::VertexId endpoint){
      return method==VolumeConnectionMethod::adaptive_cleaving&&
          std::abs(source_distances[endpoint])<=safe_warp_radius[endpoint];
    };
    const bool warp_first=warpable(endpoints[0]),warp_second=warpable(endpoints[1]);
    if(warp_first||warp_second){
      if(warp_first&&warp_second){
        const double first_distance=std::abs(source_distances[endpoints[0]]);
        const double second_distance=std::abs(source_distances[endpoints[1]]);
        vertex=first_distance<second_distance||(first_distance==second_distance&&endpoints[0]<endpoints[1])
            ?endpoints[0]:endpoints[1];
      }else vertex=warp_first?endpoints[0]:endpoints[1];
      scene.connected_volume_vertices[vertex]=project_to_surface(source_vertices[vertex]);
    }else if (fraction <= snap_epsilon) vertex=endpoints[0];
    else if (fraction >= 1.0-snap_epsilon) vertex=endpoints[1];
    else {
      vertex=scene.connected_volume_vertices.size();
      scene.connected_volume_vertices.push_back(intersection);
      scene.connected_volume_vertex_kinds.push_back(ConnectedVertexKind::surface_intersection);
      scene.connected_volume_source_edges.push_back({{endpoints[0],endpoints[1]}});
      scene.connected_volume_surface_vertices.push_back(1U);
    }
    if(vertex<source_vertices.size()){
      scene.connected_volume_surface_vertices[vertex]=1U;
      scene.connected_volume_vertex_kinds[vertex]=ConnectedVertexKind::snapped_surface;
      auto& source_edge=scene.connected_volume_source_edges[vertex];
      if(source_edge[0]==std::numeric_limits<tetra::VertexId>::max())
        source_edge={{endpoints[0],endpoints[1]}};
    }
    crossings.push_back({key,vertex});
  }
  const auto crossing_vertex=[&crossings](tetra::VertexId first,tetra::VertexId second) {
    const auto key=pack_edge(first,second);
    const auto found=std::lower_bound(crossings.begin(),crossings.end(),key,
        [](const CrossedEdge& crossing,PackedEdge value){return crossing.key<value;});
    return found->vertex;
  };
  const auto append_cell=[&scene](std::array<std::size_t,4> cell,tetra::TetId parent,bool boundary) {
    const auto& vertices=scene.connected_volume_vertices;
    double volume=signed_six_volume(vertices[cell[0]],vertices[cell[1]],vertices[cell[2]],vertices[cell[3]]);
    if(std::abs(volume)<1.0e-15)return;
    if(volume<0.0)std::swap(cell[0],cell[1]);
    scene.connected_volume_tetrahedra.push_back(cell);
    scene.connected_volume_parents.push_back(parent);
    scene.connected_volume_boundary.push_back(boundary?1U:0U);
    scene.connected_volume_regions.push_back(boundary
        ?ConnectedCellRegion::boundary_connector
        :ConnectedCellRegion::hierarchy_core);
  };
  const auto append_selected_prism=[&](const std::array<std::size_t,6>& mapping,
                                       std::size_t constrained_quad_count,
                                       bool quadrilateral_surface,
                                       tetra::TetId parent){
    PrismCandidateScore best;
    best.surface_energy=std::numeric_limits<double>::infinity();
    best.minimum_mean_ratio=-1.0;
    best.minimum_volume_surface_quality=-1.0;
    best.minimum_dihedral_sine=-1.0;
    best.maximum_dihedral_degrees=180.0;
    best.template_index=prism_templates.size();
    PrismCandidateScore fixed_score;
    bool fixed_score_ready=false;
    for(std::size_t candidate_index=0;candidate_index<prism_templates.size();++candidate_index){
      bool compatible=true;
      for(std::size_t quad=0;quad<constrained_quad_count;++quad)
        compatible=compatible&&prism_quad_diagonals[candidate_index][quad]==
                               prism_quad_diagonals[0][quad];
      if(!compatible)continue;
      PrismCandidateScore score;
      score.template_index=candidate_index;
      for(const auto& local_tet:prism_templates[candidate_index]){
        std::array<tetra::Vec3,4> points{};
        for(std::size_t corner=0;corner<4;++corner)
          points[corner]=scene.connected_volume_vertices[mapping[local_tet[corner]]];
        auto quality=evaluate_tetrahedron_quality(points);
        if(quality.signed_six_volume<0.0){
          std::swap(points[0],points[1]);
          quality=evaluate_tetrahedron_quality(points);
        }
        if(quality.signed_six_volume<=1.0e-15){
          compatible=false;
          break;
        }
        score.minimum_mean_ratio=std::min(score.minimum_mean_ratio,quality.mean_ratio);
        score.minimum_volume_surface_quality=std::min(
            score.minimum_volume_surface_quality,quality.volume_surface_longest_edge);
        score.minimum_dihedral_sine=std::min(
            score.minimum_dihedral_sine,quality.minimum_dihedral_sine);
        score.maximum_dihedral_degrees=std::max(
            score.maximum_dihedral_degrees,quality.maximum_dihedral_degrees);
      }
      if(!compatible)continue;
      if(quadrilateral_surface){
        const auto diagonal=prism_quad_diagonals[candidate_index][2];
        constexpr std::array<std::size_t,4> surface_vertices{{1,2,4,5}};
        std::array<std::size_t,2> other{};
        std::size_t cursor{};
        for(const auto vertex:surface_vertices)
          if(vertex!=diagonal[0]&&vertex!=diagonal[1])other[cursor++]=vertex;
        for(const auto third:other){
          const std::array<tetra::Vec3,3> triangle{{
              scene.connected_volume_vertices[mapping[diagonal[0]]],
              scene.connected_volume_vertices[mapping[diagonal[1]]],
              scene.connected_volume_vertices[mapping[third]]}};
          score.surface_energy+=triangle_angle_energy(triangle);
        }
      }else{
        score.surface_energy=triangle_angle_energy({{
            scene.connected_volume_vertices[mapping[3]],
            scene.connected_volume_vertices[mapping[4]],
            scene.connected_volume_vertices[mapping[5]]}});
      }
      if(candidate_index==0){
        fixed_score=score;
        fixed_score_ready=true;
      }else if(fixed_score_ready&&stencil_selection_objective!=StencilSelectionObjective::volume){
        const double retained_fraction=stencil_selection_objective==
            StencilSelectionObjective::balanced?1.0:0.98;
        if(score.minimum_mean_ratio<fixed_score.minimum_mean_ratio*retained_fraction||
           score.minimum_volume_surface_quality<
               fixed_score.minimum_volume_surface_quality*retained_fraction||
           score.minimum_dihedral_sine<fixed_score.minimum_dihedral_sine*retained_fraction||
           score.maximum_dihedral_degrees>fixed_score.maximum_dihedral_degrees+2.0)
          continue;
      }
      if(best.template_index==prism_templates.size()||
         better_prism_candidate(score,best,stencil_selection_objective))best=score;
    }
    if(best.template_index==prism_templates.size())best.template_index=0;
    ++scene.selected_stencil_cells;
    if(best.template_index!=0)++scene.alternate_stencil_cells;
    for(const auto& local_tet:prism_templates[best.template_index]){
      std::array<std::size_t,4> cell{};
      for(std::size_t corner=0;corner<4;++corner)cell[corner]=mapping[local_tet[corner]];
      append_cell(cell,parent,true);
    }
  };

  scene.connected_volume_tetrahedra.reserve(mesh.conforming_volume().size()*5);
  scene.connected_volume_parents.reserve(mesh.conforming_volume().size()*5);
  scene.connected_volume_boundary.reserve(mesh.conforming_volume().size()*5);
  scene.connected_volume_regions.reserve(mesh.conforming_volume().size()*5);
  for (const auto id : mesh.conforming_volume().addresses()) {
    const auto& tet = mesh.tetrahedron(id).vertices;
    std::array<bool,4> inside{};
    std::size_t inside_count{};
    for(std::size_t corner=0;corner<4;++corner){
      inside[corner]=source_distances[tet[corner]]<=0.0;
      inside_count+=inside[corner]?1U:0U;
    }
    if(inside_count==0)continue;
    if(inside_count==4){
      append_cell({tet[0],tet[1],tet[2],tet[3]},id,false);
      continue;
    }

    if(quality_stencils){
      // Direct two-material clipped-tetrahedron templates. Ordering both
      // material partitions by their global hierarchy vertex ids makes every
      // shared-face diagonal deterministic from either incident parent.
      std::array<std::size_t,4> inside_corners{},outside_corners{};
      std::size_t inside_cursor{},outside_cursor{};
      for(std::size_t corner=0;corner<4;++corner)
        (inside[corner]?inside_corners[inside_cursor++]:outside_corners[outside_cursor++])=corner;
      const auto by_vertex=[&](std::size_t first,std::size_t second){return tet[first]<tet[second];};
      std::sort(inside_corners.begin(),inside_corners.begin()+static_cast<std::ptrdiff_t>(inside_cursor),by_vertex);
      std::sort(outside_corners.begin(),outside_corners.begin()+static_cast<std::ptrdiff_t>(outside_cursor),by_vertex);
      const auto cut=[&](std::size_t inside_corner,std::size_t outside_corner){
        return crossing_vertex(tet[inside_corner],tet[outside_corner]);
      };
      if(inside_count==1){
        const auto a=inside_corners[0];
        append_cell({tet[a],cut(a,outside_corners[0]),cut(a,outside_corners[1]),
                     cut(a,outside_corners[2])},id,true);
      }else if(inside_count==2){
        const auto a=inside_corners[0],b=inside_corners[1];
        const auto c=outside_corners[0],d=outside_corners[1];
        const auto ac=cut(a,c),ad=cut(a,d),bc=cut(b,c),bd=cut(b,d);
        if(stencil_construction==StencilConstruction::selected)
          append_selected_prism({{tet[a],ac,ad,tet[b],bc,bd}},2,true,id);
        else{
          append_cell({tet[a],ac,ad,tet[b]},id,true);
          append_cell({ac,ad,tet[b],bc},id,true);
          append_cell({ad,tet[b],bc,bd},id,true);
        }
      }else if(inside_count==3){
        const auto a=inside_corners[0],b=inside_corners[1],c=inside_corners[2];
        const auto d=outside_corners[0];
        const auto ad=cut(a,d),bd=cut(b,d),cd=cut(c,d);
        if(stencil_construction==StencilConstruction::selected)
          append_selected_prism({{tet[a],tet[b],tet[c],ad,bd,cd}},3,false,id);
        else{
          append_cell({tet[a],tet[b],tet[c],ad},id,true);
          append_cell({tet[b],tet[c],ad,bd},id,true);
          append_cell({tet[c],ad,bd,cd},id,true);
        }
      }
      continue;
    }

    std::vector<std::size_t> clipped_vertices;
    clipped_vertices.reserve(6);
    for(std::size_t corner=0;corner<4;++corner)if(inside[corner])clipped_vertices.push_back(tet[corner]);
    for(const auto edge:edges)if(inside[edge[0]]!=inside[edge[1]])
      clipped_vertices.push_back(crossing_vertex(tet[edge[0]],tet[edge[1]]));
    std::sort(clipped_vertices.begin(),clipped_vertices.end());
    clipped_vertices.erase(std::unique(clipped_vertices.begin(),clipped_vertices.end()),clipped_vertices.end());
    tetra::Vec3 centre{};
    for(const auto vertex:clipped_vertices)centre=centre+scene.connected_volume_vertices[vertex];
    centre=centre/static_cast<double>(clipped_vertices.size());
    const std::size_t centre_id=scene.connected_volume_vertices.size();
    scene.connected_volume_vertices.push_back(centre);
    scene.connected_volume_vertex_kinds.push_back(ConnectedVertexKind::stencil_interior);
    scene.connected_volume_source_edges.push_back(
        {{std::numeric_limits<tetra::VertexId>::max(),
          std::numeric_limits<tetra::VertexId>::max()}});
    scene.connected_volume_surface_vertices.push_back(0U);

    std::vector<std::array<std::size_t,2>> interface_edges;
    interface_edges.reserve(4);
    for(const auto face:faces){
      std::vector<std::size_t> polygon;
      std::vector<std::size_t> face_crossings;
      polygon.reserve(4);
      for(std::size_t index=0;index<3;++index){
        const auto current=face[index],next=face[(index+1)%3];
        if(inside[current]&&(polygon.empty()||polygon.back()!=tet[current]))polygon.push_back(tet[current]);
        if(inside[current]!=inside[next]){
          const auto cut=crossing_vertex(tet[current],tet[next]);
          if(polygon.empty()||polygon.back()!=cut)polygon.push_back(cut);
          face_crossings.push_back(cut);
        }
      }
      if(polygon.size()>1&&polygon.front()==polygon.back())polygon.pop_back();
      triangulate_polygon(polygon,[&](const auto& triangle){append_cell({centre_id,triangle[0],triangle[1],triangle[2]},id,true);});
      std::sort(face_crossings.begin(),face_crossings.end());
      face_crossings.erase(std::unique(face_crossings.begin(),face_crossings.end()),face_crossings.end());
      if(face_crossings.size()==2)interface_edges.push_back({face_crossings[0],face_crossings[1]});
    }

    std::vector<std::size_t> interface_polygon;
    if(!interface_edges.empty()){
      const auto start=std::min_element(interface_edges.begin(),interface_edges.end(),
          [](const auto& first,const auto& second){return std::min(first[0],first[1])<std::min(second[0],second[1]);});
      const std::size_t first=std::min((*start)[0],(*start)[1]);
      std::size_t previous=std::numeric_limits<std::size_t>::max(),current=first;
      for(std::size_t step=0;step<interface_edges.size();++step){
        interface_polygon.push_back(current);
        std::array<std::size_t,2> neighbours{};
        std::size_t neighbour_count{};
        for(const auto edge:interface_edges){
          if(edge[0]==current)neighbours[neighbour_count++]=edge[1];
          else if(edge[1]==current)neighbours[neighbour_count++]=edge[0];
        }
        if(neighbour_count==0)break;
        std::sort(neighbours.begin(),neighbours.begin()+static_cast<std::ptrdiff_t>(neighbour_count));
        std::size_t next=neighbours[0];
        if(next==previous&&neighbour_count>1)next=neighbours[1];
        previous=current;
        current=next;
        if(current==first)break;
      }
    }
    triangulate_polygon(interface_polygon,[&](const auto& triangle){append_cell({centre_id,triangle[0],triangle[1],triangle[2]},id,true);});
  }
}

// Improve the authoritative exterior of the connected volume in place. The
// surface and the boundary tetrahedra share these vertex indices, so every
// accepted move is immediately a conforming volume edit rather than a render
// mesh overlay. All adjacency is stored in compact offset/index arrays.
void optimize_connected_volume_boundary(PreparedScene& scene,
                                         const tetra::Sphere& sphere) {
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  using Edge=std::array<std::size_t,2>;
  const std::size_t vertex_count=scene.connected_volume_vertices.size();
  const std::size_t tet_count=scene.connected_volume_tetrahedra.size();
  if(vertex_count==0||tet_count==0)return;

  std::vector<ConnectedFace> all_faces;
  all_faces.reserve(tet_count*4);
  for(std::size_t tet_index=0;tet_index<tet_count;++tet_index){
    const auto& tet=scene.connected_volume_tetrahedra[tet_index];
    for(const auto face:faces){
      std::array<std::size_t,3> vertices{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      auto key=vertices;
      std::sort(key.begin(),key.end());
      all_faces.push_back({key,vertices,tet_index});
    }
  }
  std::sort(all_faces.begin(),all_faces.end(),[](const ConnectedFace& first,
                                                 const ConnectedFace& second){
    return first.key<second.key;
  });
  std::vector<std::array<std::size_t,3>> boundary_faces;
  std::vector<Edge> boundary_edges;
  for(std::size_t begin=0;begin<all_faces.size();){
    std::size_t end=begin+1;
    while(end<all_faces.size()&&all_faces[end].key==all_faces[begin].key)++end;
    if(end-begin==1&&std::ranges::all_of(all_faces[begin].vertices,[&](std::size_t vertex){
         return scene.connected_volume_surface_vertices[vertex]!=0U;
       })){
      auto oriented_face=all_faces[begin].vertices;
      const auto& owner=scene.connected_volume_tetrahedra[all_faces[begin].tetrahedron];
      tetra::Vec3 tet_centre{};
      for(const auto vertex:owner)tet_centre=tet_centre+scene.connected_volume_vertices[vertex];
      tet_centre=tet_centre/4.0;
      const auto a=scene.connected_volume_vertices[oriented_face[0]];
      const auto b=scene.connected_volume_vertices[oriented_face[1]];
      const auto c=scene.connected_volume_vertices[oriented_face[2]];
      const auto normal=face_normal(a,b,c);
      const auto face_centre=(a+b+c)/3.0;
      const auto inward=tet_centre-face_centre;
      if(normal.x*inward.x+normal.y*inward.y+normal.z*inward.z>0.0)
        std::swap(oriented_face[1],oriented_face[2]);
      boundary_faces.push_back(oriented_face);
      for(const auto pair:std::array<std::array<std::size_t,2>,3>{{{{0,1}},{{1,2}},{{2,0}}}}){
        Edge edge{{oriented_face[pair[0]],oriented_face[pair[1]]}};
        if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
        boundary_edges.push_back(edge);
      }
    }
    begin=end;
  }
  std::sort(boundary_edges.begin(),boundary_edges.end());
  boundary_edges.erase(std::unique(boundary_edges.begin(),boundary_edges.end()),boundary_edges.end());
  if(boundary_faces.empty())return;

  std::vector<std::size_t> neighbor_offsets(vertex_count+1),
                           incident_tet_offsets(vertex_count+1),
                           incident_face_offsets(vertex_count+1);
  for(const auto edge:boundary_edges){
    ++neighbor_offsets[edge[0]+1];
    ++neighbor_offsets[edge[1]+1];
  }
  for(const auto& tet:scene.connected_volume_tetrahedra)
    for(const auto vertex:tet)++incident_tet_offsets[vertex+1];
  for(const auto& face:boundary_faces)
    for(const auto vertex:face)++incident_face_offsets[vertex+1];
  for(std::size_t index=1;index<=vertex_count;++index){
    neighbor_offsets[index]+=neighbor_offsets[index-1];
    incident_tet_offsets[index]+=incident_tet_offsets[index-1];
    incident_face_offsets[index]+=incident_face_offsets[index-1];
  }
  scene.optimized_surface_vertices=0;
  for(std::size_t vertex=0;vertex<vertex_count;++vertex)
    if(neighbor_offsets[vertex+1]>neighbor_offsets[vertex])
      ++scene.optimized_surface_vertices;
  std::vector<std::size_t> neighbors(neighbor_offsets.back()),
                           incident_tets(incident_tet_offsets.back()),
                           incident_faces(incident_face_offsets.back());
  auto neighbor_cursor=neighbor_offsets, tet_cursor=incident_tet_offsets,
       face_cursor=incident_face_offsets;
  for(const auto edge:boundary_edges){
    neighbors[neighbor_cursor[edge[0]]++]=edge[1];
    neighbors[neighbor_cursor[edge[1]]++]=edge[0];
  }
  for(std::size_t tet_index=0;tet_index<tet_count;++tet_index)
    for(const auto vertex:scene.connected_volume_tetrahedra[tet_index])
      incident_tets[tet_cursor[vertex]++]=tet_index;
  for(std::size_t face_index=0;face_index<boundary_faces.size();++face_index)
    for(const auto vertex:boundary_faces[face_index])
      incident_faces[face_cursor[vertex]++]=face_index;

  const auto tet_quality=[&](std::size_t tet_index,std::size_t moved_vertex,
                             tetra::Vec3 candidate){
    const auto& ids=scene.connected_volume_tetrahedra[tet_index];
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)
      points[corner]=ids[corner]==moved_vertex?candidate:scene.connected_volume_vertices[ids[corner]];
    return evaluate_tetrahedron_quality(points);
  };
  std::vector<double> initial_determinants(tet_count),initial_qualities(tet_count);
  scene.minimum_connected_tet_quality_before=1.0;
  scene.minimum_connected_tet_volume_surface_quality_before=1.0;
  scene.minimum_connected_tet_dihedral_sine_before=1.0;
  for(std::size_t tet_index=0;tet_index<tet_count;++tet_index){
    const auto quality=tet_quality(tet_index,std::numeric_limits<std::size_t>::max(),{});
    initial_determinants[tet_index]=quality.signed_six_volume;
    initial_qualities[tet_index]=quality.mean_ratio;
    scene.minimum_connected_tet_quality_before=
        std::min(scene.minimum_connected_tet_quality_before,quality.mean_ratio);
    scene.minimum_connected_tet_volume_surface_quality_before=std::min(
        scene.minimum_connected_tet_volume_surface_quality_before,
        quality.volume_surface_longest_edge);
    scene.minimum_connected_tet_dihedral_sine_before=std::min(
        scene.minimum_connected_tet_dihedral_sine_before,quality.minimum_dihedral_sine);
  }
  const auto triangle_fairness=[&](const std::array<std::size_t,3>& ids,
                                   std::size_t moved_vertex,tetra::Vec3 candidate){
    std::array<tetra::Vec3,3> points{};
    for(std::size_t corner=0;corner<3;++corner)
      points[corner]=ids[corner]==moved_vertex?candidate:scene.connected_volume_vertices[ids[corner]];
    double energy{};
    constexpr double target=1.0471975511965977462;
    for(std::size_t corner=0;corner<3;++corner){
      const auto a=points[(corner+1)%3]-points[corner];
      const auto b=points[(corner+2)%3]-points[corner];
      const double aa=a.x*a.x+a.y*a.y+a.z*a.z;
      const double bb=b.x*b.x+b.y*b.y+b.z*b.z;
      if(aa<=1.0e-30||bb<=1.0e-30)return std::numeric_limits<double>::infinity();
      const double cosine=std::clamp((a.x*b.x+a.y*b.y+a.z*b.z)/std::sqrt(aa*bb),-1.0,1.0);
      const double difference=std::acos(cosine)-target;
      energy+=difference*difference;
    }
    return energy;
  };
  const auto valid_move=[&](std::size_t vertex,tetra::Vec3 candidate){
    for(std::size_t offset=incident_tet_offsets[vertex];offset<incident_tet_offsets[vertex+1];++offset){
      const auto tet_index=incident_tets[offset];
      const auto quality=tet_quality(tet_index,vertex,candidate);
      if(quality.signed_six_volume<=initial_determinants[tet_index]*1.0e-6||
         quality.mean_ratio<std::max(1.0e-5,initial_qualities[tet_index]*0.5))return false;
    }
    double fairness_before{},fairness_after{};
    for(std::size_t offset=incident_face_offsets[vertex];offset<incident_face_offsets[vertex+1];++offset){
      const auto& ids=boundary_faces[incident_faces[offset]];
      std::array<tetra::Vec3,3> points{};
      for(std::size_t corner=0;corner<3;++corner)
        points[corner]=ids[corner]==vertex?candidate:scene.connected_volume_vertices[ids[corner]];
      const auto normal=face_normal(points[0],points[1],points[2]);
      const double area_squared=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
      const auto centre=(points[0]+points[1]+points[2])/3.0;
      const auto outward=sphere.normal(centre);
      if(area_squared<1.0e-24||normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<=0.0)
        return false;
      fairness_before+=triangle_fairness(
          ids,std::numeric_limits<std::size_t>::max(),{});
      fairness_after+=triangle_fairness(ids,vertex,candidate);
    }
    return fairness_after<=fairness_before+1.0e-12;
  };
  const auto project_to_surface=[&](tetra::Vec3 point){return sphere.project_to_surface(point);};
  constexpr std::size_t iterations=5,line_search_steps=10;
  constexpr double relaxation=0.35;
  for(std::size_t iteration=0;iteration<iterations;++iteration){
    for(std::size_t vertex=0;vertex<vertex_count;++vertex){
      const auto degree=neighbor_offsets[vertex+1]-neighbor_offsets[vertex];
      if(degree<3)continue;
      tetra::Vec3 average{};
      for(std::size_t offset=neighbor_offsets[vertex];offset<neighbor_offsets[vertex+1];++offset)
        average=average+scene.connected_volume_vertices[neighbors[offset]];
      average=average/static_cast<double>(degree);
      const auto original=scene.connected_volume_vertices[vertex];
      const auto target=project_to_surface(original*(1.0-relaxation)+average*relaxation);
      bool accepted=false;
      double step=1.0;
      for(std::size_t attempt=0;attempt<line_search_steps;++attempt,step*=0.5){
        const auto candidate=project_to_surface(original*(1.0-step)+target*step);
        if(valid_move(vertex,candidate)){
          scene.connected_volume_vertices[vertex]=candidate;
          ++scene.optimized_volume_boundary_vertices;
          accepted=true;
          break;
        }
      }
      if(!accepted)++scene.rejected_volume_boundary_moves;
    }
  }
  scene.minimum_connected_tet_quality_after=1.0;
  scene.minimum_connected_tet_volume_surface_quality_after=1.0;
  scene.minimum_connected_tet_dihedral_sine_after=1.0;
  scene.minimum_connected_tet_dihedral_degrees_after=180.0;
  scene.maximum_connected_tet_dihedral_degrees_after=0.0;
  for(std::size_t tet_index=0;tet_index<tet_count;++tet_index){
    const auto quality=tet_quality(tet_index,std::numeric_limits<std::size_t>::max(),{});
    scene.minimum_connected_tet_quality_after=std::min(
        scene.minimum_connected_tet_quality_after,quality.mean_ratio);
    scene.minimum_connected_tet_volume_surface_quality_after=std::min(
        scene.minimum_connected_tet_volume_surface_quality_after,
        quality.volume_surface_longest_edge);
    scene.minimum_connected_tet_dihedral_sine_after=std::min(
        scene.minimum_connected_tet_dihedral_sine_after,quality.minimum_dihedral_sine);
    scene.minimum_connected_tet_dihedral_degrees_after=std::min(
        scene.minimum_connected_tet_dihedral_degrees_after,quality.minimum_dihedral_degrees);
    scene.maximum_connected_tet_dihedral_degrees_after=std::max(
        scene.maximum_connected_tet_dihedral_degrees_after,quality.maximum_dihedral_degrees);
  }
  // Stable hash of the authoritative optimized boundary. This deliberately
  // excludes cutaway state and render buffers, so scripted uncut/cut runs can
  // prove that they expose the same connected surface.
  constexpr std::uint64_t fnv_offset=1469598103934665603ULL;
  constexpr std::uint64_t fnv_prime=1099511628211ULL;
  auto hash=fnv_offset;
  const auto append_hash=[&](std::uint64_t value){
    hash^=value;
    hash*=fnv_prime;
  };
  for(std::size_t vertex=0;vertex<vertex_count;++vertex){
    if(scene.connected_volume_surface_vertices[vertex]==0U)continue;
    append_hash(vertex);
    const auto point=scene.connected_volume_vertices[vertex];
    append_hash(std::bit_cast<std::uint64_t>(point.x));
    append_hash(std::bit_cast<std::uint64_t>(point.y));
    append_hash(std::bit_cast<std::uint64_t>(point.z));
  }
  std::vector<std::array<std::size_t,3>> boundary_keys=boundary_faces;
  for(auto& face:boundary_keys)std::sort(face.begin(),face.end());
  std::sort(boundary_keys.begin(),boundary_keys.end());
  for(const auto& face:boundary_keys)
    for(const auto vertex:face)append_hash(vertex);
  scene.connected_surface_hash=hash;
}

void append_tetrahedral_layer(PreparedScene& scene, const tetra::TetMesh& mesh, const tetra::Sphere& sphere,
                              bool show_faces, bool show_surface_edges) {
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  if (surface.empty()) return;

  // Weld independently extracted triangle corners onto a single surface
  // vertex array. The field intersections agree geometrically, while the
  // quantized key absorbs harmless floating-point ordering differences.
  using WeldKey = std::array<long long, 3>;
  std::map<WeldKey, tetra::VertexId> welded;
  std::vector<tetra::Vec3> vertices;
  std::vector<std::array<tetra::VertexId, 3>> triangles;
  vertices.reserve(surface.size());
  triangles.reserve(surface.size());
  const auto vertex_id = [&welded, &vertices](tetra::Vec3 point) {
    constexpr double scale = 1.0e10;
    const WeldKey key{{std::llround(point.x*scale), std::llround(point.y*scale), std::llround(point.z*scale)}};
    const auto found = welded.find(key);
    if (found != welded.end()) return found->second;
    const auto id = static_cast<tetra::VertexId>(vertices.size());
    vertices.push_back(point);
    welded.emplace(key, id);
    return id;
  };
  for (const auto& triangle : surface) {
    const std::array<tetra::VertexId, 3> ids{{vertex_id(triangle.a), vertex_id(triangle.b), vertex_id(triangle.c)}};
    if (ids[0] != ids[1] && ids[0] != ids[2] && ids[1] != ids[2]) triangles.push_back(ids);
  }

  const auto outer_count = static_cast<tetra::VertexId>(vertices.size());
  vertices.reserve(vertices.size()*2);
  // Offset the first layer along the implicit field normal so it works for
  // curved, sharp, periodic, and height-field surfaces alike.
  const double thickness=std::min(0.02,sphere.radius*0.10);
  for (tetra::VertexId id = 0; id < outer_count; ++id) {
    vertices.push_back(vertices[id]-sphere.normal(vertices[id])*thickness);
  }

  std::vector<std::array<tetra::VertexId, 4>> tetrahedra;
  tetrahedra.reserve(triangles.size()*3);
  for (auto triangle : triangles) {
    std::sort(triangle.begin(), triangle.end());
    const auto a = triangle[0], b = triangle[1], c = triangle[2];
    const auto ai = static_cast<tetra::VertexId>(a+outer_count);
    const auto bi = static_cast<tetra::VertexId>(b+outer_count);
    const auto ci = static_cast<tetra::VertexId>(c+outer_count);
    std::array<std::array<tetra::VertexId, 4>, 3> prism{{{{a,b,c,ci}}, {{a,b,bi,ci}}, {{a,ai,bi,ci}}}};
    for (auto tet : prism) {
      if (signed_six_volume(vertices[tet[0]], vertices[tet[1]], vertices[tet[2]], vertices[tet[3]]) < 0.0)
        std::swap(tet[0], tet[1]);
      tetrahedra.push_back(tet);
    }
  }
  scene.surface_layer_tetrahedra = tetrahedra.size();
  scene.selected_count = tetrahedra.size();

  constexpr std::array<std::array<std::size_t, 3>, 4> faces{{{{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  if (show_faces || show_surface_edges) {
    std::vector<LayerFace> layer_faces;
    layer_faces.reserve(tetrahedra.size()*4);
    for (std::size_t index = 0; index < tetrahedra.size(); ++index) for (const auto& face : faces) {
      std::array<tetra::VertexId, 3> ids{{tetrahedra[index][face[0]],tetrahedra[index][face[1]],tetrahedra[index][face[2]]}};
      auto key = ids;
      std::sort(key.begin(), key.end());
      layer_faces.push_back({key, ids, index});
    }
    std::sort(layer_faces.begin(), layer_faces.end(), [](const LayerFace& a, const LayerFace& b) { return a.key < b.key; });
    for (std::size_t begin = 0; begin < layer_faces.size();) {
      std::size_t end = begin+1;
      while (end < layer_faces.size() && layer_faces[end].key == layer_faces[begin].key) ++end;
      if (end-begin == 1) {
        const auto& face = layer_faces[begin];
        const auto& tet = tetrahedra[face.tetrahedron];
        tetra::Vec3 tet_centre{};
        for (const auto id : tet) tet_centre = tet_centre+vertices[id];
        tet_centre = tet_centre/4.0;
        const auto a=vertices[face.vertices[0]], b=vertices[face.vertices[1]], c=vertices[face.vertices[2]];
        auto normal=face_normal(a,b,c);
        const tetra::Vec3 centre{(a.x+b.x+c.x)/3.0,(a.y+b.y+c.y)/3.0,(a.z+b.z+c.z)/3.0};
        const auto outward=centre-tet_centre;
        if (normal.x*outward.x+normal.y*outward.y+normal.z*outward.z < 0.0) normal={-normal.x,-normal.y,-normal.z};
        for (const auto point : {a,b,c}) scene.triangle_vertices.push_back({
            {static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
            {0.24F,0.78F,0.48F},
            {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}});
      }
      begin=end;
    }
  }
}

void append_dual_contour(PreparedScene& scene, const tetra::TetMesh& mesh, const tetra::Sphere& sphere,
                         bool show_faces, bool show_surface_edges) {
  const auto triangles = tetra::extract_dual_contour(mesh, sphere);
  scene.dual_contour_triangles = triangles.size();
  if (show_faces || show_surface_edges) for (const auto& triangle : triangles) {
    auto normal = face_normal(triangle.a, triangle.b, triangle.c);
    const tetra::Vec3 centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                             (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                             (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
    const auto outward = sphere.normal(centre);
    if (normal.x*outward.x+normal.y*outward.y+normal.z*outward.z < 0.0)
      normal = {-normal.x,-normal.y,-normal.z};
    for (const auto point : {triangle.a, triangle.b, triangle.c})
      scene.triangle_vertices.push_back({
          {static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
          {0.22F,0.72F,0.52F},
          {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}});
  }
}

void append_marching_tetrahedra(PreparedScene& scene, const tetra::TetMesh& mesh,
                                const tetra::Sphere& sphere, bool show_faces,
                                bool show_surface_edges) {
  const auto triangles = tetra::extract_isosurface(mesh, sphere);
  scene.marching_tetrahedra_triangles = triangles.size();
  if (!show_faces && !show_surface_edges) return;
  for (const auto& triangle : triangles) {
    auto normal = face_normal(triangle.a, triangle.b, triangle.c);
    const tetra::Vec3 centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                             (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                             (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
    const auto outward = sphere.normal(centre);
    if (normal.x*outward.x+normal.y*outward.y+normal.z*outward.z < 0.0)
      normal = {-normal.x,-normal.y,-normal.z};
    for (const auto point : {triangle.a, triangle.b, triangle.c})
      scene.triangle_vertices.push_back({
          {static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
          {0.32F,0.76F,0.42F},
          {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}});
  }
}

void append_four_hexahedra(PreparedScene& scene,const tetra::TetMesh& mesh,
                           const tetra::Sphere& sphere,bool show_faces,
                           bool show_surface_edges) {
  const auto triangles=tetra::extract_four_hexahedra_isosurface(mesh,sphere);
  scene.four_hexahedra_triangles=triangles.size();
  if(!show_faces&&!show_surface_edges)return;
  for(const auto& triangle:triangles){
    auto normal=face_normal(triangle.a,triangle.b,triangle.c);
    const auto centre=(triangle.a+triangle.b+triangle.c)/3.0;
    const auto outward=sphere.normal(centre);
    if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0)
      normal={-normal.x,-normal.y,-normal.z};
    for(const auto point:{triangle.a,triangle.b,triangle.c})
      scene.triangle_vertices.push_back({
          {static_cast<float>(point.x),static_cast<float>(point.y),
           static_cast<float>(point.z)},
          {0.26F,0.72F,0.56F},
          {static_cast<float>(normal.x),static_cast<float>(normal.y),
           static_cast<float>(normal.z)}});
  }
}

void append_mixed_depth_dual(PreparedScene& scene,const tetra::TetMesh& mesh,
                             const tetra::Sphere& sphere,bool show_faces,
                             bool show_surface_edges) {
  const auto triangles=tetra::extract_mixed_depth_dual_isosurface(mesh,sphere);
  scene.mixed_depth_dual_triangles=triangles.size();
  if(!show_faces&&!show_surface_edges)return;
  for(const auto& triangle:triangles){
    auto normal=face_normal(triangle.a,triangle.b,triangle.c);
    const auto centre=(triangle.a+triangle.b+triangle.c)/3.0;
    const auto outward=sphere.normal(centre);
    if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0)
      normal={-normal.x,-normal.y,-normal.z};
    for(const auto point:{triangle.a,triangle.b,triangle.c})
      scene.triangle_vertices.push_back({
          {static_cast<float>(point.x),static_cast<float>(point.y),
           static_cast<float>(point.z)},
          {0.20F,0.74F,0.60F},
          {static_cast<float>(normal.x),static_cast<float>(normal.y),
           static_cast<float>(normal.z)}});
  }
}

void build_lattice_cleaved_cells(PreparedScene& scene,const tetra::TetMesh& mesh,
                                 const tetra::Sphere& sphere) {
  // Only sign-changing leaves are replaced. For a single inside corner the
  // clipped material is one tetrahedron; the two- and three-inside cases are
  // triangular prisms with deterministic three-tetrahedron decompositions.
  // Fully inside/outside leaves remain owned by the background hierarchy.
  scene.cleaved_cells.reserve(mesh.conforming_volume().size()*2);
  const auto crossing=[&sphere](tetra::Vec3 inside,tetra::Vec3 outside){
    return sphere.edge_intersection(inside,outside);
  };
  const auto add_cell=[&scene](std::array<tetra::Vec3,4> cell){
    double volume=signed_six_volume(cell[0],cell[1],cell[2],cell[3]);
    if(std::abs(volume)<1e-15)return;
    if(volume<0.0){std::swap(cell[0],cell[1]);volume=-volume;}
    scene.cleaved_volume+=volume/6.0;
    scene.cleaved_cells.push_back(cell);
  };
  for (const auto id : mesh.conforming_volume().addresses()) {
    std::array<tetra::Vec3,4> points{};
    std::array<std::size_t,4> inside{},outside{};
    std::size_t inside_count=0,outside_count=0;
    const auto& vertices=mesh.tetrahedron(id).vertices;
    for(std::size_t index=0;index<4;++index){
      points[index]=mesh.vertices()[vertices[index]];
      if(sphere.signed_distance(points[index])<0.0)inside[inside_count++]=index;
      else outside[outside_count++]=index;
    }
    if(inside_count==1){
      const auto a=points[inside[0]];
      add_cell({a,crossing(a,points[outside[0]]),crossing(a,points[outside[1]]),crossing(a,points[outside[2]])});
    }else if(inside_count==2){
      const auto a=points[inside[0]],b=points[inside[1]];
      const auto ac=crossing(a,points[outside[0]]),ad=crossing(a,points[outside[1]]);
      const auto bc=crossing(b,points[outside[0]]),bd=crossing(b,points[outside[1]]);
      add_cell({a,ac,ad,b});
      add_cell({ac,ad,b,bc});
      add_cell({ad,b,bc,bd});
    }else if(inside_count==3){
      const auto a=points[inside[0]],b=points[inside[1]],c=points[inside[2]],d=points[outside[0]];
      const auto ad=crossing(a,d),bd=crossing(b,d),cd=crossing(c,d);
      add_cell({a,b,c,ad});
      add_cell({b,c,ad,bd});
      add_cell({c,ad,bd,cd});
    }
  }
  scene.cleaved_tetrahedra=scene.cleaved_cells.size();
}

void append_lattice_cleaving(PreparedScene& scene, const tetra::TetMesh& mesh,
                             const tetra::Sphere& sphere, bool show_faces,
                             bool show_surface_edges) {
  build_lattice_cleaved_cells(scene,mesh,sphere);
  const auto triangles = tetra::extract_isosurface(mesh, sphere);
  if (!show_faces && !show_surface_edges) return;
  for (const auto& triangle : triangles) {
    auto normal = face_normal(triangle.a, triangle.b, triangle.c);
    const tetra::Vec3 centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                             (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                             (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
    const auto outward = sphere.normal(centre);
    if (normal.x*outward.x+normal.y*outward.y+normal.z*outward.z < 0.0)
      normal = {-normal.x,-normal.y,-normal.z};
    for (const auto point : {triangle.a, triangle.b, triangle.c})
      scene.triangle_vertices.push_back({
          {static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
          {0.80F,0.58F,0.24F},
          {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}});
  }
}

struct OptimizedSurface {
  std::vector<tetra::Vec3> positions;
  std::vector<std::array<std::size_t,3>> triangles;
  std::vector<std::array<tetra::VertexId,2>> source_edges;
};

std::vector<ConnectedFace> connected_surface_boundary_faces(const PreparedScene& source){
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::vector<ConnectedFace> all_faces;
  all_faces.reserve(source.connected_volume_tetrahedra.size()*4);
  for(std::size_t tet_index=0;tet_index<source.connected_volume_tetrahedra.size();++tet_index){
    const auto& tet=source.connected_volume_tetrahedra[tet_index];
    for(const auto face:faces){
      std::array<std::size_t,3> vertices{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      auto key=vertices;std::sort(key.begin(),key.end());
      all_faces.push_back({key,vertices,tet_index});
    }
  }
  std::sort(all_faces.begin(),all_faces.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  std::vector<ConnectedFace> boundary_faces;
  for(std::size_t begin=0;begin<all_faces.size();){
    std::size_t end=begin+1;
    while(end<all_faces.size()&&all_faces[end].key==all_faces[begin].key)++end;
    if(end-begin==1&&std::ranges::all_of(all_faces[begin].vertices,[&](std::size_t vertex){
         return source.connected_volume_surface_vertices[vertex]!=0U;
       }))boundary_faces.push_back(all_faces[begin]);
    begin=end;
  }
  return boundary_faces;
}

OptimizedSurface build_optimized_surface(PreparedScene& scene,
                                         const tetra::TetMesh& mesh,
                                         const tetra::Sphere& sphere,
                                         const PreparedScene* topology_source=nullptr,
                                         const std::vector<ConnectedFace>* supplied_boundary=nullptr) {
  std::vector<tetra::Triangle> input;
  std::vector<std::array<std::array<tetra::VertexId,2>,3>> input_source_edges;
  if(topology_source!=nullptr){
    const auto owned_boundary=supplied_boundary==nullptr?
        connected_surface_boundary_faces(*topology_source):std::vector<ConnectedFace>{};
    const auto& boundary=supplied_boundary==nullptr?owned_boundary:*supplied_boundary;
    for(const auto& face:boundary){
        auto ids=face.vertices;
        auto a=topology_source->connected_volume_vertices[ids[0]];
        auto b=topology_source->connected_volume_vertices[ids[1]];
        auto c=topology_source->connected_volume_vertices[ids[2]];
        std::array<std::array<tetra::VertexId,2>,3> source_edges{{
            topology_source->connected_volume_source_edges[ids[0]],
            topology_source->connected_volume_source_edges[ids[1]],
            topology_source->connected_volume_source_edges[ids[2]]}};
        const auto normal=face_normal(a,b,c),centre=(a+b+c)/3.0;
        const auto outward=sphere.normal(centre);
        if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0){
          std::swap(b,c);std::swap(source_edges[1],source_edges[2]);
        }
        input.push_back({a,b,c});
        input_source_edges.push_back(source_edges);
    }
  }else input=tetra::extract_isosurface(mesh,sphere);
  using PointKey=std::array<long long,3>;
  struct Corner {
    PointKey key{};
    std::array<tetra::VertexId,2> source_edge{};
    tetra::Vec3 point{};
    std::size_t triangle{},corner{};
  };
  std::vector<Corner> corners;
  corners.reserve(input.size()*3);
  const auto point_key=[](tetra::Vec3 point){
    constexpr double scale=1.0e10;
    return PointKey{{std::llround(point.x*scale),std::llround(point.y*scale),std::llround(point.z*scale)}};
  };
  struct CrossingProvenance {
    PointKey key{};
    std::array<tetra::VertexId,2> edge{};
  };
  constexpr std::array<std::array<std::size_t,2>,6> local_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::vector<CrossingProvenance> provenance;
  if(input_source_edges.size()!=input.size()){
    provenance.reserve(mesh.conforming_volume().size()*2);
    for(const auto leaf:mesh.conforming_volume().addresses()){
      const auto& tet=mesh.tetrahedron(leaf).vertices;
      for(const auto local:local_edges){
        auto edge=std::array<tetra::VertexId,2>{{tet[local[0]],tet[local[1]]}};
        const auto first=mesh.vertices()[edge[0]],second=mesh.vertices()[edge[1]];
        if((sphere.signed_distance(first)<=0.0)==(sphere.signed_distance(second)<=0.0))continue;
        if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
        provenance.push_back({point_key(sphere.edge_intersection(
            mesh.vertices()[edge[0]],mesh.vertices()[edge[1]])),edge});
      }
    }
    std::sort(provenance.begin(),provenance.end(),[](const auto& a,const auto& b){
      return a.key<b.key||(a.key==b.key&&a.edge<b.edge);
    });
    provenance.erase(std::unique(provenance.begin(),provenance.end(),[](const auto& a,const auto& b){
      return a.key==b.key;
    }),provenance.end());
  }
  for(std::size_t triangle=0;triangle<input.size();++triangle){
    const std::array<tetra::Vec3,3> points{{input[triangle].a,input[triangle].b,input[triangle].c}};
    for(std::size_t corner=0;corner<3;++corner){
      const auto key=point_key(points[corner]);
      std::array<tetra::VertexId,2> edge{};
      if(input_source_edges.size()==input.size())edge=input_source_edges[triangle][corner];
      else{
        const auto source=std::lower_bound(provenance.begin(),provenance.end(),key,
            [](const CrossingProvenance& item,const PointKey& value){return item.key<value;});
        edge=source!=provenance.end()&&source->key==key?source->edge:
            std::array<tetra::VertexId,2>{{std::numeric_limits<tetra::VertexId>::max(),
                                          std::numeric_limits<tetra::VertexId>::max()}};
      }
      corners.push_back({key,edge,points[corner],triangle,corner});
    }
  }
  std::sort(corners.begin(),corners.end(),[](const Corner& a,const Corner& b){
    return a.source_edge<b.source_edge||(a.source_edge==b.source_edge&&a.key<b.key);
  });
  std::vector<tetra::Vec3> positions;
  std::vector<std::array<tetra::VertexId,2>> source_edges;
  std::vector<std::array<std::size_t,3>> triangles(input.size());
  for(std::size_t begin=0;begin<corners.size();){
    std::size_t end=begin+1;
    while(end<corners.size()&&corners[end].source_edge==corners[begin].source_edge&&
          corners[end].key==corners[begin].key)++end;
    const std::size_t vertex=positions.size();
    positions.push_back(corners[begin].point);
    source_edges.push_back(corners[begin].source_edge);
    for(std::size_t index=begin;index<end;++index)
      triangles[corners[index].triangle][corners[index].corner]=vertex;
    begin=end;
  }
  for(auto& triangle:triangles){
    const auto normal=face_normal(positions[triangle[0]],positions[triangle[1]],positions[triangle[2]]);
    const auto centre=(positions[triangle[0]]+positions[triangle[1]]+positions[triangle[2]])/3.0;
    const auto outward=sphere.normal(centre);
    if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0)std::swap(triangle[1],triangle[2]);
  }
  std::vector<std::array<std::size_t,2>> edges;
  edges.reserve(triangles.size()*3);
  for(const auto& triangle:triangles)for(auto edge:std::array<std::array<std::size_t,2>,3>{{{{triangle[0],triangle[1]}},{{triangle[1],triangle[2]}},{{triangle[2],triangle[0]}}}}){
    if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
    edges.push_back(edge);
  }
  std::sort(edges.begin(),edges.end());
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  std::vector<std::size_t> neighbor_offsets(positions.size()+1),incident_offsets(positions.size()+1);
  for(const auto& edge:edges){++neighbor_offsets[edge[0]+1];++neighbor_offsets[edge[1]+1];}
  for(const auto& triangle:triangles)for(const auto vertex:triangle)++incident_offsets[vertex+1];
  for(std::size_t index=1;index<neighbor_offsets.size();++index){
    neighbor_offsets[index]+=neighbor_offsets[index-1];
    incident_offsets[index]+=incident_offsets[index-1];
  }
  std::vector<std::size_t> neighbors(neighbor_offsets.back()),incidents(incident_offsets.back());
  auto neighbor_cursor=neighbor_offsets,incident_cursor=incident_offsets;
  for(const auto& edge:edges){neighbors[neighbor_cursor[edge[0]]++]=edge[1];neighbors[neighbor_cursor[edge[1]]++]=edge[0];}
  for(std::size_t triangle=0;triangle<triangles.size();++triangle)
    for(const auto vertex:triangles[triangle])incidents[incident_cursor[vertex]++]=triangle;
  const auto triangle_fairness=[](const std::array<tetra::Vec3,3>& points){
    double energy{};
    constexpr double target=1.0471975511965977462;
    for(std::size_t corner=0;corner<3;++corner){
      const auto a=points[(corner+1)%3]-points[corner];
      const auto b=points[(corner+2)%3]-points[corner];
      const double aa=a.x*a.x+a.y*a.y+a.z*a.z;
      const double bb=b.x*b.x+b.y*b.y+b.z*b.z;
      if(aa<=1.0e-30||bb<=1.0e-30)return std::numeric_limits<double>::infinity();
      const double cosine=std::clamp((a.x*b.x+a.y*b.y+a.z*b.z)/std::sqrt(aa*bb),-1.0,1.0);
      const double difference=std::acos(cosine)-target;
      energy+=difference*difference;
    }
    return energy;
  };
  const auto valid_move=[&](std::size_t vertex,tetra::Vec3 candidate){
    double fairness_before{},fairness_after{};
    for(std::size_t offset=incident_offsets[vertex];offset<incident_offsets[vertex+1];++offset){
      const auto& ids=triangles[incidents[offset]];
      std::array<tetra::Vec3,3> points{{positions[ids[0]],positions[ids[1]],positions[ids[2]]}};
      fairness_before+=triangle_fairness(points);
      for(std::size_t corner=0;corner<3;++corner)if(ids[corner]==vertex)points[corner]=candidate;
      fairness_after+=triangle_fairness(points);
      const auto normal=face_normal(points[0],points[1],points[2]);
      const double area2=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
      const auto centre=(points[0]+points[1]+points[2])/3.0;
      const auto outward=sphere.normal(centre);
      if(area2<1e-20||normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<=1e-14)return false;
    }
    return fairness_after<=fairness_before+1.0e-12;
  };
  constexpr std::size_t iterations=5,line_search_steps=10;
  constexpr double relaxation=0.35;
  for(std::size_t iteration=0;iteration<iterations;++iteration){
    for(std::size_t vertex=0;vertex<positions.size();++vertex){
      tetra::Vec3 average{};
      const auto degree=neighbor_offsets[vertex+1]-neighbor_offsets[vertex];
      if(degree<3)continue;
      for(std::size_t offset=neighbor_offsets[vertex];offset<neighbor_offsets[vertex+1];++offset)
        average=average+positions[neighbors[offset]];
      average=average/static_cast<double>(degree);
      const auto original=positions[vertex];
      auto target=original*(1.0-relaxation)+average*relaxation;
      target=sphere.project_to_surface(target);
      bool accepted=false;
      double step=1.0;
      for(std::size_t attempt=0;attempt<line_search_steps;++attempt,step*=0.5){
        auto candidate=original*(1.0-step)+target*step;
        candidate=sphere.project_to_surface(candidate);
        if(valid_move(vertex,candidate)){
          positions[vertex]=candidate;
          accepted=true;
          break;
        }
      }
      if(!accepted)++scene.rejected_surface_moves;
    }
  }
  scene.optimized_surface_vertices=positions.size();
  return {std::move(positions),std::move(triangles),std::move(source_edges)};
}

std::uint64_t hash_indexed_surface(const OptimizedSurface& surface){
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  std::uint64_t hash=offset;
  const auto append=[&](std::uint64_t value){hash^=value;hash*=prime;};
  for(const auto point:surface.positions){
    append(std::bit_cast<std::uint64_t>(point.x));
    append(std::bit_cast<std::uint64_t>(point.y));
    append(std::bit_cast<std::uint64_t>(point.z));
  }
  for(const auto triangle:surface.triangles)
    for(const auto vertex:triangle)append(vertex);
  return hash;
}

void append_surface_optimization(PreparedScene& scene, const tetra::TetMesh& mesh,
                                 const tetra::Sphere& sphere, bool show_faces,
                                 bool show_surface_edges) {
  PreparedScene topology;
  build_adaptive_cleaved_volume(topology,mesh,sphere,
                                VolumeConnectionMethod::quality_stencils,
                                StencilConstruction::fixed,
                                StencilSelectionObjective::balanced);
  auto surface=build_optimized_surface(scene,mesh,sphere,&topology);
  scene.standalone_surface_hash=hash_indexed_surface(surface);
  if(!show_faces&&!show_surface_edges)return;
  for(const auto& ids:surface.triangles){
    const auto a=surface.positions[ids[0]],b=surface.positions[ids[1]],
               c=surface.positions[ids[2]];
    const auto normal=face_normal(a,b,c);
    for(const auto point:{a,b,c})scene.triangle_vertices.push_back({
        {static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
        {0.70F,0.36F,0.82F},
        {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}});
  }
}

// Preserve the independently optimized surface exactly and absorb its
// displacement in a thin, topology-matched tetrahedral shell.  The inner
// front remains anchored to the hierarchy edges, so the existing cleaving
// stencils stay embedded in their source cells and every fully-inside cell is
// copied unchanged.
void build_fixed_surface_shell(PreparedScene& scene,const tetra::TetMesh& mesh,
                               const tetra::Sphere& sphere,
                               StencilSelectionObjective objective){
  build_adaptive_cleaved_volume(scene,mesh,sphere,
                                VolumeConnectionMethod::quality_stencils,
                                StencilConstruction::fixed,objective);
  auto boundary_faces=connected_surface_boundary_faces(scene);
  auto outer=build_optimized_surface(scene,mesh,sphere,&scene,&boundary_faces);
  scene.standalone_surface_hash=hash_indexed_surface(outer);
  scene.connected_surface_hash=scene.standalone_surface_hash;
  scene.hybrid_shell_vertices=outer.positions.size();

  constexpr std::array<std::array<std::size_t,3>,4> face_corners{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  using SourceEdge=std::array<tetra::VertexId,2>;
  struct SourceVertex { SourceEdge edge{};std::size_t vertex{}; };
  std::vector<SourceVertex> source_vertices;
  source_vertices.reserve(boundary_faces.size());
  for(const auto& face:boundary_faces)for(const auto vertex:face.vertices){
    const auto edge=scene.connected_volume_source_edges[vertex];
    if(edge[0]!=std::numeric_limits<tetra::VertexId>::max())
      source_vertices.push_back({edge,vertex});
  }
  std::sort(source_vertices.begin(),source_vertices.end(),[](const auto& a,const auto& b){
    return a.edge<b.edge||(a.edge==b.edge&&a.vertex<b.vertex);
  });
  source_vertices.erase(std::unique(source_vertices.begin(),source_vertices.end(),[](const auto& a,const auto& b){
    return a.edge==b.edge;
  }),source_vertices.end());

  std::vector<std::size_t> inner(outer.positions.size(),std::numeric_limits<std::size_t>::max());
  for(std::size_t index=0;index<outer.source_edges.size();++index){
    const auto found=std::lower_bound(source_vertices.begin(),source_vertices.end(),outer.source_edges[index],
        [](const SourceVertex& value,const SourceEdge& edge){return value.edge<edge;});
    if(found!=source_vertices.end()&&found->edge==outer.source_edges[index])inner[index]=found->vertex;
  }

  std::vector<std::size_t> incident_offsets(scene.connected_volume_vertices.size()+1);
  for(const auto& tet:scene.connected_volume_tetrahedra)
    for(const auto vertex:tet)++incident_offsets[vertex+1];
  for(std::size_t index=1;index<incident_offsets.size();++index)
    incident_offsets[index]+=incident_offsets[index-1];
  std::vector<std::size_t> incident_tets(incident_offsets.back());
  auto incident_cursor=incident_offsets;
  for(std::size_t tet_index=0;tet_index<scene.connected_volume_tetrahedra.size();++tet_index)
    for(const auto vertex:scene.connected_volume_tetrahedra[tet_index])
      incident_tets[incident_cursor[vertex]++]=tet_index;
  for(std::size_t index=0;index<inner.size();++index){
    const auto vertex=inner[index];
    if(vertex==std::numeric_limits<std::size_t>::max()){
      ++scene.hybrid_missing_provenance;
      ++scene.hybrid_failed_prisms;
      continue;
    }
    const auto edge=outer.source_edges[index];
    const bool first_inside=sphere.signed_distance(mesh.vertices()[edge[0]])<=0.0;
    const auto target=mesh.vertices()[first_inside?edge[0]:edge[1]];
    const auto original=scene.connected_volume_vertices[vertex];
    bool accepted=false;
    // Forty percent gives the shell enough normal depth without pulling the
    // connector close to its inside hierarchy endpoint. Deterministic line
    // search below reduces it in the rare incident-cell failure case.
    double fraction=0.40;
    for(std::size_t attempt=0;attempt<8;++attempt,fraction*=0.5){
      const auto candidate=original*(1.0-fraction)+target*fraction;
      bool valid=sphere.signed_distance(candidate)<-1.0e-12;
      for(std::size_t offset=incident_offsets[vertex];offset<incident_offsets[vertex+1];++offset){
        const auto tet_index=incident_tets[offset];
        auto ids=scene.connected_volume_tetrahedra[tet_index];
        std::array<tetra::Vec3,4> points{};
        for(std::size_t corner=0;corner<4;++corner)
          points[corner]=ids[corner]==vertex?candidate:scene.connected_volume_vertices[ids[corner]];
        valid=valid&&evaluate_tetrahedron_quality(points).signed_six_volume>1.0e-15;
      }
      if(valid){
        scene.connected_volume_vertices[vertex]=candidate;
        scene.hybrid_recovery_steps+=attempt;
        accepted=true;
        break;
      }
    }
    if(!accepted){++scene.hybrid_inset_failures;++scene.hybrid_failed_prisms;}
  }

  const std::size_t outer_base=scene.connected_volume_vertices.size();
  scene.connected_volume_vertices.insert(scene.connected_volume_vertices.end(),
                                         outer.positions.begin(),outer.positions.end());
  for(std::size_t index=0;index<outer.positions.size();++index){
    scene.connected_volume_vertex_kinds.push_back(ConnectedVertexKind::fixed_outer_surface);
    scene.connected_volume_source_edges.push_back(outer.source_edges[index]);
    scene.connected_volume_surface_vertices.push_back(1U);
  }
  for(const auto vertex:inner)if(vertex!=std::numeric_limits<std::size_t>::max())
    scene.connected_volume_surface_vertices[vertex]=0U;

  std::vector<ConnectedFace> sorted_boundary=boundary_faces;
  std::sort(sorted_boundary.begin(),sorted_boundary.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  for(auto triangle:outer.triangles){
    std::sort(triangle.begin(),triangle.end());
    const auto a=triangle[0],b=triangle[1],c=triangle[2];
    if(inner[a]==std::numeric_limits<std::size_t>::max()||
       inner[b]==std::numeric_limits<std::size_t>::max()||
       inner[c]==std::numeric_limits<std::size_t>::max()){
      ++scene.hybrid_failed_prisms;continue;
    }
    std::array<std::size_t,3> inner_key{{inner[a],inner[b],inner[c]}};
    std::sort(inner_key.begin(),inner_key.end());
    const auto owner=std::lower_bound(sorted_boundary.begin(),sorted_boundary.end(),inner_key,
        [](const ConnectedFace& face,const auto& key){return face.key<key;});
    if(owner==sorted_boundary.end()||owner->key!=inner_key){
      ++scene.hybrid_missing_inner_faces;
      ++scene.hybrid_failed_prisms;continue;
    }
    const auto parent=scene.connected_volume_parents[owner->tetrahedron];
    const std::size_t ao=outer_base+a,bo=outer_base+b,co=outer_base+c;
    const std::array<std::size_t,6> prism{{ao,bo,co,inner[a],inner[b],inner[c]}};
    std::array<std::array<std::size_t,4>,3> cells{};
    for(std::size_t tet=0;tet<3;++tet)
      for(std::size_t corner=0;corner<4;++corner)
        cells[tet][corner]=prism[prism_templates[0][tet][corner]];
    bool valid=true;
    for(auto& cell:cells){
      std::array<tetra::Vec3,4> points{};
      for(std::size_t corner=0;corner<4;++corner)points[corner]=scene.connected_volume_vertices[cell[corner]];
      double volume=evaluate_tetrahedron_quality(points).signed_six_volume;
      if(std::abs(volume)<=1.0e-15){valid=false;break;}
      if(volume<0.0)std::swap(cell[0],cell[1]);
    }
    if(!valid){++scene.hybrid_degenerate_prisms;++scene.hybrid_failed_prisms;continue;}
    for(const auto& cell:cells){
      scene.connected_volume_tetrahedra.push_back(cell);
      scene.connected_volume_parents.push_back(parent);
      scene.connected_volume_boundary.push_back(1U);
      scene.connected_volume_regions.push_back(ConnectedCellRegion::outer_shell);
      ++scene.hybrid_shell_tetrahedra;
    }
  }

  scene.minimum_connected_tet_quality_before=1.0;
  scene.minimum_connected_tet_quality_after=1.0;
  scene.minimum_connected_tet_volume_surface_quality_before=1.0;
  scene.minimum_connected_tet_volume_surface_quality_after=1.0;
  for(const auto& ids:scene.connected_volume_tetrahedra){
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)points[corner]=scene.connected_volume_vertices[ids[corner]];
    const auto quality=evaluate_tetrahedron_quality(points);
    if(quality.signed_six_volume<=1.0e-15)++scene.hybrid_failed_prisms;
    scene.minimum_connected_tet_quality_before=std::min(scene.minimum_connected_tet_quality_before,quality.mean_ratio);
    scene.minimum_connected_tet_volume_surface_quality_before=std::min(
        scene.minimum_connected_tet_volume_surface_quality_before,quality.volume_surface_longest_edge);
  }
  scene.minimum_connected_tet_quality_after=scene.minimum_connected_tet_quality_before;
  scene.minimum_connected_tet_volume_surface_quality_after=
      scene.minimum_connected_tet_volume_surface_quality_before;
  std::vector<std::array<std::size_t,3>> final_faces;
  final_faces.reserve(scene.connected_volume_tetrahedra.size()*4);
  for(const auto& tet:scene.connected_volume_tetrahedra)for(const auto face:face_corners){
    std::array<std::size_t,3> key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
    std::sort(key.begin(),key.end());final_faces.push_back(key);
  }
  std::sort(final_faces.begin(),final_faces.end());
  std::size_t unmatched{},outer_unmatched{};
  bool incidence_valid=true;
  for(std::size_t begin=0;begin<final_faces.size();){
    std::size_t end=begin+1;
    while(end<final_faces.size()&&final_faces[end]==final_faces[begin])++end;
    incidence_valid=incidence_valid&&end-begin<=2;
    if(end-begin==1){
      ++unmatched;
      if(std::ranges::all_of(final_faces[begin],[&](std::size_t vertex){
           return scene.connected_volume_surface_vertices[vertex]!=0U;
         }))++outer_unmatched;
    }
    begin=end;
  }
  scene.hybrid_volume_valid=scene.hybrid_failed_prisms==0&&incidence_valid&&
      unmatched==outer_unmatched&&outer_unmatched==outer.triangles.size()&&
      scene.hybrid_shell_tetrahedra==outer.triangles.size()*3&&
      scene.connected_volume_regions.size()==scene.connected_volume_tetrahedra.size();
  scene.hybrid_unmatched_faces=unmatched-outer_unmatched;
}

bool is_material(const tetra::TetMesh& mesh, tetra::TetId id, const tetra::Sphere& sphere,
                 tetra::SurfaceRelation relation, MaterialRule rule) {
  const auto& tet = mesh.tetrahedron(id).vertices;
  std::size_t inside_vertices = 0;
  tetra::Vec3 centroid{};
  for (const auto vertex : tet) {
    const auto point = mesh.vertices()[vertex];
    if (sphere.signed_distance(point) <= 0.0) ++inside_vertices;
    centroid = centroid + point;
  }
  switch (rule) {
    case MaterialRule::all_vertices_inside: return inside_vertices == 4;
    case MaterialRule::centroid_inside: return sphere.signed_distance(centroid / 4.0) <= 0.0;
    case MaterialRule::majority_vertices_inside: return inside_vertices >= 3;
    case MaterialRule::any_overlap: return relation != tetra::SurfaceRelation::outside;
    case MaterialRule::variational: return false;
    case MaterialRule::variational_faithful: return false;
    case MaterialRule::variational_smooth: return false;
  }
  return false;
}

struct VolumeCutClassification {
  std::vector<tetra::TetId> material_tetrahedra;
  std::vector<tetra::TetId> boundary_tetrahedra;
  std::vector<std::array<tetra::VertexId,3>> boundary_faces;
};

VolumeCutClassification classify_volume_cut_cells(
    const tetra::TetMesh& mesh, const tetra::Sphere& sphere, MaterialRule rule,
    std::span<const tetra::SurfaceRelation> relations) {
  VolumeCutClassification result;
  const auto leaves = mesh.conforming_volume().addresses();
  result.material_tetrahedra.reserve(leaves.size());
  std::vector<MaterialFace> faces;
  faces.reserve(leaves.size()*4);
  constexpr std::array<std::array<std::size_t, 3>, 4> face_corners{{
      {{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
  const auto variational=is_variational_material_rule(rule)
      ? tetra::build_whole_cell_cut(mesh,sphere,whole_cell_options(rule)) : tetra::WholeCellCut{};
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    const auto id = leaves[index];
    if (is_variational_material_rule(rule) ? !variational.selected(index)
                                       : !is_material(mesh,id,sphere,relations[index],rule)) continue;
    result.material_tetrahedra.push_back(id);
    const auto& tet = mesh.tetrahedron(id).vertices;
    for (const auto face : face_corners) {
      std::array<tetra::VertexId, 3> vertices{{tet[face[0]], tet[face[1]], tet[face[2]]}};
      auto key = vertices;
      std::sort(key.begin(), key.end());
      faces.push_back({key, vertices, id});
    }
  }
  std::sort(faces.begin(), faces.end(), [](const MaterialFace& first, const MaterialFace& second) {
    return first.key < second.key;
  });
  for (std::size_t begin = 0; begin < faces.size();) {
    std::size_t end = begin+1;
    while (end < faces.size() && faces[end].key == faces[begin].key) ++end;
    if (end-begin == 1) {
      result.boundary_tetrahedra.push_back(faces[begin].tetrahedron);
      result.boundary_faces.push_back(faces[begin].key);
    }
    begin = end;
  }
  std::sort(result.boundary_tetrahedra.begin(), result.boundary_tetrahedra.end());
  result.boundary_tetrahedra.erase(
      std::unique(result.boundary_tetrahedra.begin(), result.boundary_tetrahedra.end()),
      result.boundary_tetrahedra.end());
  return result;
}

void append_selected_volume(PreparedScene& scene, const tetra::TetMesh& mesh,
                            std::span<const tetra::TetId> material_tetrahedra,
                            std::span<const tetra::TetId> boundary_tetrahedra,
                            std::span<const std::array<tetra::VertexId,3>> material_boundary_faces,
                            double x_cut_position, bool show_edges, bool show_faces,
                            bool show_material_boundary) {
  constexpr std::array<std::array<std::size_t, 3>, 4> faces{{
      {{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
  constexpr std::array<float, 3> internal_colour{0.18F, 0.62F, 0.78F};
  constexpr std::array<float, 3> boundary_colour{0.94F, 0.43F, 0.12F};
  std::vector<MaterialFace> visible_faces;
  std::vector<PackedEdge> internal_edges, boundary_edges;
  if (show_faces || show_edges) visible_faces.reserve(material_tetrahedra.size()*4);
  if (show_edges) {
    internal_edges.reserve(material_tetrahedra.size()*6);
    boundary_edges.reserve(material_tetrahedra.size()*2);
  }
  for (const auto id : material_tetrahedra) {
    const auto& tet = mesh.tetrahedron(id).vertices;
    if(std::ranges::any_of(tet,[&](tetra::VertexId vertex){
      return mesh.vertices()[vertex].x>x_cut_position;
    }))continue;
    const bool boundary = std::binary_search(boundary_tetrahedra.begin(), boundary_tetrahedra.end(), id);
    if (show_faces || show_edges) for (const auto face : faces) {
      std::array<tetra::VertexId, 3> vertices{{tet[face[0]], tet[face[1]], tet[face[2]]}};
      auto key = vertices;
      std::sort(key.begin(), key.end());
      visible_faces.push_back({key, vertices, id});
    }
    if (show_edges) {
      auto& destination = boundary ? boundary_edges : internal_edges;
      for (std::size_t first = 0; first < 4; ++first)
        for (std::size_t second = first+1; second < 4; ++second)
          destination.push_back(pack_edge(tet[first], tet[second]));
    }
  }
  if (show_edges) {
    for (auto* edges : {&internal_edges, &boundary_edges}) {
      std::sort(edges->begin(), edges->end());
      edges->erase(std::unique(edges->begin(), edges->end()), edges->end());
    }
    std::erase_if(internal_edges, [&boundary_edges](PackedEdge edge) {
      return std::binary_search(boundary_edges.begin(), boundary_edges.end(), edge);
    });
    scene.volume_internal_edges = internal_edges.size();
    scene.volume_boundary_edges = boundary_edges.size();
  }
  if (show_faces || show_edges) {
    std::sort(visible_faces.begin(), visible_faces.end(), [](const MaterialFace& first, const MaterialFace& second) {
      return first.key < second.key;
    });
    for (std::size_t begin = 0; begin < visible_faces.size();) {
      std::size_t end = begin+1;
      while (end < visible_faces.size() && visible_faces[end].key == visible_faces[begin].key) ++end;
      if (end-begin == 1) {
        const auto& face = visible_faces[begin];
        // An independently generated isosurface remains the authoritative
        // smooth exterior. In that mode submit only faces exposed by hiding
        // whole cells at the cut plane, not the coarse outer boundary of the
        // selected hierarchy-cell union.
        const bool original_material_boundary=std::binary_search(
            material_boundary_faces.begin(),material_boundary_faces.end(),face.key);
        if(original_material_boundary&&!show_material_boundary){begin=end;continue;}
        const auto& tet = mesh.tetrahedron(face.tetrahedron).vertices;
        tetra::Vec3 tet_centre{};
        for (const auto vertex : tet) tet_centre = tet_centre + mesh.vertices()[vertex];
        tet_centre = tet_centre/4.0;
        const auto a=mesh.vertices()[face.vertices[0]], b=mesh.vertices()[face.vertices[1]], c=mesh.vertices()[face.vertices[2]];
        auto normal=face_normal(a,b,c);
        const auto face_centre=(a+b+c)/3.0;
        if (normal.x*(face_centre.x-tet_centre.x)+normal.y*(face_centre.y-tet_centre.y)+normal.z*(face_centre.z-tet_centre.z)<0.0)
          normal={-normal.x,-normal.y,-normal.z};
        const bool boundary = std::binary_search(boundary_tetrahedra.begin(), boundary_tetrahedra.end(), face.tetrahedron);
        const auto& colour = boundary ? boundary_colour : internal_colour;
        const std::array<tetra::Vec3,3> points{{a,b,c}};
        for (std::size_t corner=0;corner<3;++corner) {
          const auto point=points[corner];
          SceneVertex vertex{{static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
                             {colour[0],colour[1],colour[2]},
                             {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}};
          vertex.diagnostics[0]=-1.0F;
          // Bit 3 asks the fragment shader for a depth-masked wire-only face.
          vertex.edge_flags=show_faces ? 7.0F : 15.0F;
          vertex.barycentric[corner]=1.0F;
          scene.triangle_vertices.push_back(vertex);
        }
        ++scene.visible_volume_face_triangles;
      }
      begin=end;
    }
  }
}

void append_connected_volume(PreparedScene& scene, const tetra::TetMesh& mesh,
                             const tetra::Sphere&,
                             double x_cut_position, bool show_edges, bool show_volume_faces,
                             bool show_connected_surface,
                             bool show_connected_surface_edges) {
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  constexpr std::array<float,3> internal_colour{0.18F,0.62F,0.78F};
  constexpr std::array<float,3> boundary_colour{0.94F,0.43F,0.12F};
  constexpr std::array<float,3> surface_colour{0.32F,0.76F,0.42F};
  using Edge=std::array<std::size_t,2>;
  std::vector<ConnectedFace> visible_faces;
  std::vector<Edge> internal_edges,boundary_edges,surface_edges;
  const bool need_faces=show_volume_faces||show_connected_surface||show_edges||
                        show_connected_surface_edges;
  if(need_faces)
    visible_faces.reserve(scene.connected_volume_tetrahedra.size()*4);
  tetra::TetId previous_parent=tetra::invalid_tet;
  bool parent_visible{};
  for(std::size_t index=0;index<scene.connected_volume_tetrahedra.size();++index){
    const auto& tet=scene.connected_volume_tetrahedra[index];
    // A cleaved parent may contain many stencil tetrahedra. Cutting those
    // children independently tears the parent into a jagged, apparently
    // cracked subset. Keep the hierarchy leaf as the visibility unit so all
    // of its replacement cells are shown or hidden together.
    const auto parent=scene.connected_volume_parents[index];
    if(parent!=previous_parent){
      parent_visible=std::ranges::none_of(mesh.tetrahedron(parent).vertices,[&](tetra::VertexId vertex){
        return mesh.vertices()[vertex].x>x_cut_position;
      });
      previous_parent=parent;
    }
    if(!parent_visible)continue;
    if(need_faces)for(const auto face:faces){
      std::array<std::size_t,3> vertices{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      auto key=vertices;
      std::sort(key.begin(),key.end());
      visible_faces.push_back({key,vertices,index});
    }
  }
  std::sort(visible_faces.begin(),visible_faces.end(),[](const ConnectedFace& first,const ConnectedFace& second){
    return first.key<second.key;
  });
  for(std::size_t begin=0;begin<visible_faces.size();){
    std::size_t end=begin+1;
    while(end<visible_faces.size()&&visible_faces[end].key==visible_faces[begin].key)++end;
    if(end-begin==1){
      const auto& face=visible_faces[begin];
      const bool interface_face=std::ranges::all_of(face.vertices,[&](std::size_t vertex){
        return scene.connected_volume_surface_vertices[vertex]!=0U;
      });
      if(show_edges&&!interface_face){
        auto& destination=scene.connected_volume_boundary[face.tetrahedron]?boundary_edges:internal_edges;
        for(const auto pair:std::array<std::array<std::size_t,2>,3>{{{{0,1}},{{1,2}},{{2,0}}}}){
          Edge edge{{face.vertices[pair[0]],face.vertices[pair[1]]}};
          if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
          destination.push_back(edge);
        }
      }
      if(show_connected_surface_edges&&interface_face){
        for(const auto pair:std::array<std::array<std::size_t,2>,3>{{{{0,1}},{{1,2}},{{2,0}}}}){
          Edge edge{{face.vertices[pair[0]],face.vertices[pair[1]]}};
          if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
          surface_edges.push_back(edge);
        }
      }
      if((interface_face&&!show_connected_surface&&!show_connected_surface_edges)||
         (!interface_face&&!show_volume_faces&&!show_edges)){
        begin=end;
        continue;
      }
      const auto& tet=scene.connected_volume_tetrahedra[face.tetrahedron];
      tetra::Vec3 tet_centre{};
      for(const auto vertex:tet)tet_centre=tet_centre+scene.connected_volume_vertices[vertex];
      tet_centre=tet_centre/4.0;
      std::array<tetra::Vec3,3> points{{
          scene.connected_volume_vertices[face.vertices[0]],
          scene.connected_volume_vertices[face.vertices[1]],
          scene.connected_volume_vertices[face.vertices[2]]}};
      auto normal=face_normal(points[0],points[1],points[2]);
      const auto face_centre=(points[0]+points[1]+points[2])/3.0;
      if(normal.x*(face_centre.x-tet_centre.x)+normal.y*(face_centre.y-tet_centre.y)+normal.z*(face_centre.z-tet_centre.z)<0.0){
        // Keep the submitted winding consistent with the outward normal.
        // Vulkan back-face culling uses winding, not the normal attribute.
        std::swap(points[1],points[2]);
        normal={-normal.x,-normal.y,-normal.z};
      }
      const auto& colour=interface_face?surface_colour:
          (scene.connected_volume_boundary[face.tetrahedron]?boundary_colour:internal_colour);
      for(std::size_t corner=0;corner<3;++corner){
        const auto point=points[corner];
        SceneVertex vertex{{static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)},
                           {colour[0],colour[1],colour[2]},
                           {static_cast<float>(normal.x),static_cast<float>(normal.y),static_cast<float>(normal.z)}};
        // -2 identifies the authoritative external surface of the connected
        // volume; -1 identifies ordinary volume/cutaway faces.
        vertex.diagnostics[0]=interface_face?-2.0F:-1.0F;
        // Bit 3 asks the fragment shader for a depth-masked wire-only volume
        // face. Surface faces retain their independent face/edge controls.
        vertex.edge_flags=(!interface_face&&!show_volume_faces)?15.0F:7.0F;
        vertex.barycentric[corner]=1.0F;
        scene.triangle_vertices.push_back(vertex);
      }
      ++scene.visible_volume_face_triangles;
    }
    begin=end;
  }
  if(show_edges){
    for(auto* edges_to_sort:{&internal_edges,&boundary_edges}){
      std::sort(edges_to_sort->begin(),edges_to_sort->end());
      edges_to_sort->erase(std::unique(edges_to_sort->begin(),edges_to_sort->end()),edges_to_sort->end());
    }
    std::erase_if(internal_edges,[&boundary_edges](const Edge& edge){
      return std::binary_search(boundary_edges.begin(),boundary_edges.end(),edge);
    });
    scene.volume_internal_edges=internal_edges.size();
    scene.volume_boundary_edges=boundary_edges.size();
  }
  if(show_connected_surface_edges){
    std::sort(surface_edges.begin(),surface_edges.end());
    surface_edges.erase(std::unique(surface_edges.begin(),surface_edges.end()),surface_edges.end());
    scene.connected_surface_edges=surface_edges.size();
  }
}

void append_screen_space_edges(PreparedScene& scene, bool show_surface_edges,
                               bool show_volume_edges, bool show_surface_faces,
                               bool show_volume_faces) {
  struct WireEdge {
    std::array<std::uint32_t,6> key{};
    SceneVertex first{};
    SceneVertex second{};
    int priority{};
  };
  std::vector<WireEdge> edges;
  edges.reserve(scene.triangle_vertices.size());
  const auto endpoint_key=[](const SceneVertex& vertex){
    std::array<std::uint32_t,3> key{};
    for(std::size_t axis=0;axis<3;++axis){
      const float coordinate=vertex.position[axis]==0.0F?0.0F:vertex.position[axis];
      key[axis]=std::bit_cast<std::uint32_t>(coordinate);
    }
    return key;
  };
  constexpr std::array<std::array<std::size_t,2>,3> pairs{{{{1,2}},{{2,0}},{{0,1}}}};
  for(std::size_t triangle=0;triangle+2<scene.triangle_vertices.size();triangle+=3){
    const auto* vertices=scene.triangle_vertices.data()+triangle;
    const bool connected_surface=vertices[0].diagnostics[0]<-1.5F;
    const bool volume_face=vertices[0].diagnostics[0]<-0.5F&&!connected_surface;
    if((volume_face&&!show_volume_edges)||(!volume_face&&!show_surface_edges))continue;
    const int mask=static_cast<int>(vertices[0].edge_flags+0.5F)&7;
    const bool solid=volume_face?show_volume_faces:show_surface_faces;
    std::array<float,3> colour{};
    int priority=1;
    if(!solid){
      colour={0.78F,0.92F,1.0F};
    }else if(connected_surface){
      colour={0.015F,0.055F,0.028F};
      priority=3;
    }else if(volume_face&&vertices[0].colour[0]>0.7F){
      colour={0.18F,0.055F,0.015F};
      priority=2;
    }else{
      colour={0.025F,0.04F,0.055F};
    }
    for(std::size_t edge_index=0;edge_index<3;++edge_index){
      if((mask&(1<<edge_index))==0)continue;
      SceneVertex first=vertices[pairs[edge_index][0]];
      SceneVertex second=vertices[pairs[edge_index][1]];
      auto first_key=endpoint_key(first),second_key=endpoint_key(second);
      if(second_key<first_key){
        std::swap(first,second);
        std::swap(first_key,second_key);
      }
      first.colour[0]=second.colour[0]=colour[0];
      first.colour[1]=second.colour[1]=colour[1];
      first.colour[2]=second.colour[2]=colour[2];
      WireEdge edge;
      std::copy(first_key.begin(),first_key.end(),edge.key.begin());
      std::copy(second_key.begin(),second_key.end(),edge.key.begin()+3);
      edge.first=first;
      edge.second=second;
      edge.priority=priority;
      edges.push_back(edge);
    }
  }
  std::sort(edges.begin(),edges.end(),[](const WireEdge& first,const WireEdge& second){
    return first.key<second.key||(first.key==second.key&&first.priority>second.priority);
  });
  scene.surface_line_vertices.reserve(edges.size()*2);
  for(std::size_t begin=0;begin<edges.size();){
    std::size_t end=begin+1;
    while(end<edges.size()&&edges[end].key==edges[begin].key)++end;
    scene.surface_line_vertices.push_back(edges[begin].first);
    scene.surface_line_vertices.push_back(edges[begin].second);
    begin=end;
  }
}

}  // namespace

SurfaceQualityEvaluation evaluate_surface_quality(
    const PreparedScene& scene,const tetra::Sphere& surface,
    unsigned int reference_grid_resolution) {
  if(reference_grid_resolution<2U)
    throw std::invalid_argument("surface quality grid resolution must be at least two");
  struct QualityTriangle {
    std::array<tetra::Vec3,3> points{};
    tetra::Vec3 minimum{},maximum{},centre{};
  };
  struct BvhNode {
    tetra::Vec3 minimum{},maximum{};
    std::size_t begin{},count{};
    std::size_t left{std::numeric_limits<std::size_t>::max()};
    std::size_t right{std::numeric_limits<std::size_t>::max()};
  };
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){
    return a.x*b.x+a.y*b.y+a.z*b.z;
  };
  const auto length_squared=[&](tetra::Vec3 value){return dot(value,value);};
  const auto coordinate=[](tetra::Vec3 point,std::size_t axis){
    return axis==0U?point.x:(axis==1U?point.y:point.z);
  };
  std::vector<QualityTriangle> triangles;
  triangles.reserve(scene.triangle_vertices.size()/3U);
  SurfaceQualityEvaluation result;
  result.triangle_count=scene.triangle_vertices.size()/3U;
  for(std::size_t begin=0;begin+2U<scene.triangle_vertices.size();begin+=3U){
    QualityTriangle triangle;
    for(std::size_t corner=0;corner<3U;++corner){
      const auto& source=scene.triangle_vertices[begin+corner];
      triangle.points[corner]={source.position[0],source.position[1],
                               source.position[2]};
    }
    triangle.minimum=triangle.maximum=triangle.points[0];
    for(std::size_t corner=1U;corner<3U;++corner){
      const auto point=triangle.points[corner];
      triangle.minimum.x=std::min(triangle.minimum.x,point.x);
      triangle.minimum.y=std::min(triangle.minimum.y,point.y);
      triangle.minimum.z=std::min(triangle.minimum.z,point.z);
      triangle.maximum.x=std::max(triangle.maximum.x,point.x);
      triangle.maximum.y=std::max(triangle.maximum.y,point.y);
      triangle.maximum.z=std::max(triangle.maximum.z,point.z);
    }
    triangle.centre=(triangle.points[0]+triangle.points[1]+triangle.points[2])/3.0;
    const auto ab=triangle.points[1]-triangle.points[0];
    const auto ac=triangle.points[2]-triangle.points[0];
    const tetra::Vec3 area_normal{
        ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,
        ab.x*ac.y-ab.y*ac.x};
    const double area_squared=area_normal.x*area_normal.x+
        area_normal.y*area_normal.y+area_normal.z*area_normal.z;
    if(area_squared<=1.0e-24){
      ++result.degenerate_triangle_count;
      continue;
    }
    triangles.push_back(triangle);
  }
  if(triangles.empty())return result;

  std::vector<std::size_t> indices(triangles.size());
  std::iota(indices.begin(),indices.end(),0U);
  std::vector<BvhNode> nodes;
  nodes.reserve(triangles.size()*2U);
  const auto build=[&](auto&& self,std::size_t begin,std::size_t end)->std::size_t{
    const auto node_index=nodes.size();
    nodes.push_back({});
    auto minimum=triangles[indices[begin]].minimum;
    auto maximum=triangles[indices[begin]].maximum;
    for(std::size_t position=begin+1U;position<end;++position){
      const auto& triangle=triangles[indices[position]];
      minimum.x=std::min(minimum.x,triangle.minimum.x);
      minimum.y=std::min(minimum.y,triangle.minimum.y);
      minimum.z=std::min(minimum.z,triangle.minimum.z);
      maximum.x=std::max(maximum.x,triangle.maximum.x);
      maximum.y=std::max(maximum.y,triangle.maximum.y);
      maximum.z=std::max(maximum.z,triangle.maximum.z);
    }
    nodes[node_index].minimum=minimum;
    nodes[node_index].maximum=maximum;
    nodes[node_index].begin=begin;
    nodes[node_index].count=end-begin;
    if(end-begin<=8U)return node_index;
    const auto extent=maximum-minimum;
    const std::size_t axis=extent.x>=extent.y&&extent.x>=extent.z?0U:
        (extent.y>=extent.z?1U:2U);
    const auto middle=begin+(end-begin)/2U;
    std::nth_element(
        indices.begin()+static_cast<std::ptrdiff_t>(begin),
        indices.begin()+static_cast<std::ptrdiff_t>(middle),
        indices.begin()+static_cast<std::ptrdiff_t>(end),
        [&](std::size_t left,std::size_t right){
          return coordinate(triangles[left].centre,axis)<
                 coordinate(triangles[right].centre,axis);
        });
    nodes[node_index].left=self(self,begin,middle);
    nodes[node_index].right=self(self,middle,end);
    nodes[node_index].count=0U;
    return node_index;
  };
  static_cast<void>(build(build,0U,indices.size()));

  const auto box_distance_squared=[](tetra::Vec3 point,const BvhNode& node){
    const auto axis=[](double value,double minimum,double maximum){
      if(value<minimum)return minimum-value;
      if(value>maximum)return value-maximum;
      return 0.0;
    };
    const double x=axis(point.x,node.minimum.x,node.maximum.x);
    const double y=axis(point.y,node.minimum.y,node.maximum.y);
    const double z=axis(point.z,node.minimum.z,node.maximum.z);
    return x*x+y*y+z*z;
  };
  const auto triangle_distance_squared=[&](tetra::Vec3 point,
                                            const QualityTriangle& triangle){
    const auto a=triangle.points[0],b=triangle.points[1],c=triangle.points[2];
    const auto ab=b-a,ac=c-a,ap=point-a;
    const double d1=dot(ab,ap),d2=dot(ac,ap);
    if(d1<=0.0&&d2<=0.0)return length_squared(ap);
    const auto bp=point-b;
    const double d3=dot(ab,bp),d4=dot(ac,bp);
    if(d3>=0.0&&d4<=d3)return length_squared(bp);
    const double vc=d1*d4-d3*d2;
    if(vc<=0.0&&d1>=0.0&&d3<=0.0){
      const double v=d1/(d1-d3);
      return length_squared(point-(a+ab*v));
    }
    const auto cp=point-c;
    const double d5=dot(ab,cp),d6=dot(ac,cp);
    if(d6>=0.0&&d5<=d6)return length_squared(cp);
    const double vb=d5*d2-d1*d6;
    if(vb<=0.0&&d2>=0.0&&d6<=0.0){
      const double w=d2/(d2-d6);
      return length_squared(point-(a+ac*w));
    }
    const double va=d3*d6-d5*d4;
    if(va<=0.0&&(d4-d3)>=0.0&&(d5-d6)>=0.0){
      const double w=(d4-d3)/((d4-d3)+(d5-d6));
      return length_squared(point-(b+(c-b)*w));
    }
    const double denominator=1.0/(va+vb+vc);
    const double v=vb*denominator,w=vc*denominator;
    return length_squared(point-(a+ab*v+ac*w));
  };
  std::vector<std::size_t> query_stack;
  query_stack.reserve(64U);
  const auto nearest_distance_squared=[&](tetra::Vec3 point){
    double best=std::numeric_limits<double>::infinity();
    query_stack.clear();
    query_stack.push_back(0U);
    while(!query_stack.empty()){
      const auto node_index=query_stack.back();query_stack.pop_back();
      const auto& node=nodes[node_index];
      if(box_distance_squared(point,node)>=best)continue;
      if(node.count!=0U){
        for(std::size_t position=node.begin;position<node.begin+node.count;++position)
          best=std::min(best,triangle_distance_squared(
              point,triangles[indices[position]]));
      }else{
        const double left_distance=box_distance_squared(point,nodes[node.left]);
        const double right_distance=box_distance_squared(point,nodes[node.right]);
        if(left_distance<right_distance){
          if(right_distance<best)query_stack.push_back(node.right);
          if(left_distance<best)query_stack.push_back(node.left);
        }else{
          if(left_distance<best)query_stack.push_back(node.left);
          if(right_distance<best)query_stack.push_back(node.right);
        }
      }
    }
    return best;
  };

  constexpr double radians_to_degrees=57.295779513082320876;
  for(const auto& triangle:triangles){
    const auto& p=triangle.points;
    const std::array<double,3> lengths{{
        std::sqrt(length_squared(p[1]-p[0])),
        std::sqrt(length_squared(p[2]-p[1])),
        std::sqrt(length_squared(p[0]-p[2]))}};
    const auto [shortest,longest]=std::minmax_element(lengths.begin(),lengths.end());
    const double aspect=*shortest>1.0e-15?*longest/ *shortest:
        std::numeric_limits<double>::infinity();
    result.mean_triangle_edge_aspect_ratio+=aspect;
    result.maximum_triangle_edge_aspect_ratio=std::max(
        result.maximum_triangle_edge_aspect_ratio,aspect);
    const auto area_normal=face_normal(p[0],p[1],p[2]);
    const auto normal=area_normal/std::sqrt(length_squared(area_normal));
    const double error=std::acos(std::clamp(
        dot(normal,surface.normal(triangle.centre)),-1.0,1.0))*radians_to_degrees;
    result.mean_normal_error_degrees+=error;
    result.maximum_normal_error_degrees=std::max(
        result.maximum_normal_error_degrees,error);
    const std::array<tetra::Vec3,7> mesh_samples{{
        p[0],p[1],p[2],(p[0]+p[1])/2.0,(p[1]+p[2])/2.0,
        (p[2]+p[0])/2.0,triangle.centre}};
    for(const auto sample:mesh_samples)
      result.mesh_to_implicit_distance=std::max(
          result.mesh_to_implicit_distance,std::abs(surface.signed_distance(sample)));
  }
  result.mean_triangle_edge_aspect_ratio/=static_cast<double>(triangles.size());
  result.mean_normal_error_degrees/=static_cast<double>(triangles.size());

  const unsigned int resolution=reference_grid_resolution;
  const std::size_t side=static_cast<std::size_t>(resolution)+1U;
  std::vector<double> field(side*side*side);
  const auto grid_index=[=](unsigned int x,unsigned int y,unsigned int z){
    return (static_cast<std::size_t>(z)*side+y)*side+x;
  };
  const auto grid_point=[=](unsigned int x,unsigned int y,unsigned int z){
    const double inverse=1.0/static_cast<double>(resolution);
    return tetra::Vec3{static_cast<double>(x)*inverse,
                       static_cast<double>(y)*inverse,
                       static_cast<double>(z)*inverse};
  };
  for(unsigned int z=0;z<=resolution;++z)
    for(unsigned int y=0;y<=resolution;++y)
      for(unsigned int x=0;x<=resolution;++x)
        field[grid_index(x,y,z)]=surface.signed_distance(grid_point(x,y,z));
  const auto include_reference=[&](tetra::Vec3 point){
    ++result.implicit_reference_samples;
    result.implicit_to_mesh_distance=std::max(
        result.implicit_to_mesh_distance,
        std::sqrt(nearest_distance_squared(point)));
  };
  for(unsigned int z=0;z<=resolution;++z)
    for(unsigned int y=0;y<=resolution;++y)
      for(unsigned int x=0;x<=resolution;++x){
        const auto first=grid_point(x,y,z);
        const double first_distance=field[grid_index(x,y,z)];
        const auto edge=[&](unsigned int nx,unsigned int ny,unsigned int nz){
          const auto second=grid_point(nx,ny,nz);
          const double second_distance=field[grid_index(nx,ny,nz)];
          if(first_distance==0.0)include_reference(first);
          if(second_distance==0.0)include_reference(second);
          if((first_distance<0.0)!=(second_distance<0.0))
            include_reference(surface.edge_intersection(first,second));
        };
        if(x<resolution)edge(x+1U,y,z);
        if(y<resolution)edge(x,y+1U,z);
        if(z<resolution)edge(x,y,z+1U);
      }
  result.sampled_hausdorff_distance=std::max(
      result.mesh_to_implicit_distance,result.implicit_to_mesh_distance);
  result.valid=result.implicit_reference_samples!=0U&&
      std::isfinite(result.sampled_hausdorff_distance)&&
      std::isfinite(result.mean_normal_error_degrees)&&
      std::isfinite(result.mean_triangle_edge_aspect_ratio);
  return result;
}

SurfaceGeometryHashes surface_geometry_hashes(const PreparedScene& scene) {
  using PointKey=std::array<std::uint32_t,3>;
  using TriangleKey=std::array<PointKey,3>;
  using EdgeKey=std::array<PointKey,2>;
  using MaterialTriangleKey=std::array<std::uint32_t,13>;
  const auto point_key=[](const SceneVertex& vertex){
    PointKey key{};
    for(std::size_t axis=0;axis<3U;++axis){
      const float coordinate=vertex.position[axis]==0.0F?0.0F:vertex.position[axis];
      key[axis]=std::bit_cast<std::uint32_t>(coordinate);
    }
    return key;
  };
  std::vector<TriangleKey> triangles;
  std::vector<MaterialTriangleKey> material_triangles;
  triangles.reserve(scene.triangle_vertices.size()/3U);
  material_triangles.reserve(scene.triangle_vertices.size()/3U);
  for(std::size_t begin=0;begin+2U<scene.triangle_vertices.size();begin+=3U){
    const auto* vertices=scene.triangle_vertices.data()+begin;
    const float marker=vertices[0].diagnostics[0];
    const bool diagnostic_volume_face=marker<-0.5F&&marker>=-1.5F;
    if(diagnostic_volume_face)continue;
    TriangleKey key{{point_key(vertices[0]),point_key(vertices[1]),point_key(vertices[2])}};
    const TriangleKey second{{key[1],key[2],key[0]}};
    const TriangleKey third{{key[2],key[0],key[1]}};
    key=std::min({key,second,third});
    triangles.push_back(key);
    MaterialTriangleKey material{};
    std::size_t output{};
    for(const auto& point:key)
      for(const auto coordinate:point)material[output++]=coordinate;
    for(std::size_t channel=0;channel<3U;++channel){
      const float colour=vertices[0].colour[channel]==0.0F
          ?0.0F:vertices[0].colour[channel];
      material[output++]=std::bit_cast<std::uint32_t>(colour);
    }
    material[output]=marker<-1.5F?2U:0U;
    material_triangles.push_back(material);
  }
  std::sort(triangles.begin(),triangles.end());
  std::sort(material_triangles.begin(),material_triangles.end());
  std::vector<EdgeKey> edge_incidence;
  edge_incidence.reserve(triangles.size()*3U);
  constexpr std::array<std::array<std::size_t,2>,3> edge_corners{{
      {{0U,1U}},{{1U,2U}},{{2U,0U}}}};
  for(const auto& triangle:triangles){
    for(const auto corners:edge_corners){
      EdgeKey edge{{triangle[corners[0]],triangle[corners[1]]}};
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      edge_incidence.push_back(edge);
    }
  }
  std::sort(edge_incidence.begin(),edge_incidence.end());
  auto edges=edge_incidence;
  std::sort(edges.begin(),edges.end());
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  std::vector<EdgeKey> wire_edges;
  wire_edges.reserve(scene.surface_line_vertices.size()/2U);
  for(std::size_t begin=0;begin+1U<scene.surface_line_vertices.size();begin+=2U){
    const auto* vertices=scene.surface_line_vertices.data()+begin;
    const float marker=vertices[0].diagnostics[0];
    const bool diagnostic_volume_face=marker<-0.5F&&marker>=-1.5F;
    if(diagnostic_volume_face)continue;
    EdgeKey edge{{point_key(vertices[0]),point_key(vertices[1])}};
    if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
    wire_edges.push_back(edge);
  }
  std::sort(wire_edges.begin(),wire_edges.end());
  wire_edges.erase(std::unique(wire_edges.begin(),wire_edges.end()),
                   wire_edges.end());
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto hash_keys=[=](const auto& keys){
    std::uint64_t hash=offset;
    const auto append=[&](std::uint64_t value){hash^=value;hash*=prime;};
    append(keys.size());
    for(const auto& key:keys)
      for(const auto& point:key)
        for(const auto coordinate:point)append(coordinate);
    return hash;
  };
  const auto hash_scalars=[=](const auto& keys){
    std::uint64_t hash=offset;
    const auto append=[&](std::uint64_t value){hash^=value;hash*=prime;};
    append(keys.size());
    for(const auto& key:keys)
      for(const auto value:key)append(value);
    return hash;
  };
  return {
      .triangle_hash=hash_keys(triangles),
      .edge_hash=hash_keys(edges),
      .edge_incidence_hash=hash_keys(edge_incidence),
      .material_boundary_hash=hash_scalars(material_triangles),
      .wire_edge_hash=hash_keys(wire_edges),
      .triangle_count=triangles.size(),
      .edge_count=edges.size(),
      .edge_incidence_count=edge_incidence.size(),
      .wire_edge_count=wire_edges.size()};
}

SurfaceDrawChunkStorage::SurfaceDrawChunkStorage(
    std::size_t chunk_capacity,
    SurfaceDrawChunkStrategy strategy,
    std::size_t large_patch_threshold)
    :chunk_capacity_(chunk_capacity),strategy_(strategy),
     large_patch_threshold_(large_patch_threshold) {
  if(chunk_capacity_==0U)
    throw std::invalid_argument("surface draw chunk capacity must be positive");
  if(strategy_==SurfaceDrawChunkStrategy::hybrid_large_patches&&
     (large_patch_threshold_==0U||large_patch_threshold_>chunk_capacity_))
    throw std::invalid_argument(
        "surface draw large-patch threshold must fit a chunk");
}

bool SurfaceDrawChunkStorage::is_large_patch(
    std::size_t triangle_count) const noexcept {
  return strategy_==SurfaceDrawChunkStrategy::hybrid_large_patches&&
      triangle_count>=large_patch_threshold_;
}

std::size_t SurfaceDrawChunkStorage::required_chunks(
    std::span<const SurfacePatchRecord> patches) const {
  std::size_t chunks{},used{};
  for(const auto& patch:patches){
    if(patch.triangle_count==0U)continue;
    if(is_large_patch(patch.triangle_count)){
      used=0U;
      const auto dedicated=patch.triangle_count/chunk_capacity_+
          (patch.triangle_count%chunk_capacity_!=0U?1U:0U);
      if(dedicated>std::numeric_limits<std::size_t>::max()-chunks)
        throw std::overflow_error("surface draw chunk count overflow");
      chunks+=dedicated;
      continue;
    }
    std::size_t remaining=patch.triangle_count;
    while(remaining!=0U){
      if(used==0U){
        if(chunks==std::numeric_limits<std::size_t>::max())
          throw std::overflow_error("surface draw chunk count overflow");
        ++chunks;
      }
      const auto count=std::min(remaining,chunk_capacity_-used);
      used+=count;
      remaining-=count;
      if(used==chunk_capacity_)used=0U;
    }
  }
  return chunks;
}

void SurfaceDrawChunkStorage::normalize_free_ranges() {
  std::sort(free_ranges_.begin(),free_ranges_.end(),
            [](const auto& left,const auto& right){
              return left.begin_slot<right.begin_slot;
            });
  std::size_t output{};
  for(const auto range:free_ranges_){
    if(range.slot_count==0U)continue;
    if(output!=0U&&free_ranges_[output-1U].begin_slot+
           free_ranges_[output-1U].slot_count>=range.begin_slot){
      auto& previous=free_ranges_[output-1U];
      previous.slot_count=std::max(
          previous.begin_slot+previous.slot_count,
          range.begin_slot+range.slot_count)-previous.begin_slot;
    }else free_ranges_[output++]=range;
  }
  free_ranges_.resize(output);
}

void SurfaceDrawChunkStorage::release_active_slots() {
  free_ranges_.reserve(free_ranges_.size()+chunks_.size());
  for(const auto& chunk:chunks_){
    free_ranges_.push_back({chunk.arena_slot,1U});
    ++metrics_.released_slots;
  }
  normalize_free_ranges();
  chunks_.clear();
  segments_.clear();
}

std::size_t SurfaceDrawChunkStorage::allocate_slot() {
  if(!free_ranges_.empty()){
    const auto slot=free_ranges_.front().begin_slot++;
    if(--free_ranges_.front().slot_count==0U)free_ranges_.erase(free_ranges_.begin());
    ++metrics_.reused_slots;
    return slot;
  }
  const auto slot=arena_.size()/chunk_capacity_;
  arena_.resize(arena_.size()+chunk_capacity_);
  ++metrics_.allocated_slots;
  return slot;
}

void SurfaceDrawChunkStorage::finish_metrics(
    std::size_t triangle_count,
    std::chrono::steady_clock::time_point start) {
  metrics_.patch_segments=segments_.size();
  metrics_.strategy=strategy_;
  metrics_.large_patch_threshold=large_patch_threshold_;
  metrics_.triangles=triangle_count;
  metrics_.active_chunks=chunks_.size();
  metrics_.retained_slots=arena_.size()/chunk_capacity_;
  metrics_.free_slots=0U;
  for(const auto range:free_ranges_)metrics_.free_slots+=range.slot_count;
  metrics_.fragmented_slots=chunks_.size()*chunk_capacity_-triangle_count;
  metrics_.fragmentation_bytes=
      metrics_.fragmented_slots*sizeof(tetra::Triangle);
  metrics_.draw_calls=chunks_.size();
  metrics_.occupancy=chunks_.empty()?0.0:
      static_cast<double>(triangle_count)/
          static_cast<double>(chunks_.size()*chunk_capacity_);
  metrics_.chunk_splits=0U;
  metrics_.chunk_merges=0U;
  for(const auto& segment:segments_){
    if(segment.source_triangle_offset!=0U)++metrics_.chunk_splits;
    const auto slot_begin=chunks_[segment.chunk_index].arena_slot*chunk_capacity_;
    if(segment.source_triangle_offset==0U&&segment.triangle_begin!=slot_begin)
      ++metrics_.chunk_merges;
  }
  metrics_.retained_bytes=
      chunks_.capacity()*sizeof(SurfaceDrawChunkRecord)+
      segments_.capacity()*sizeof(SurfaceDrawPatchSegment)+
      free_ranges_.capacity()*sizeof(SurfaceDrawChunkFreeRange)+
      arena_.capacity()*sizeof(tetra::Triangle)+
      retained_patches_.capacity()*sizeof(RetainedPatch);
  metrics_.pack_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-start).count();
}

void SurfaceDrawChunkStorage::compact(
    std::span<const SurfacePatchRecord> patches,
    std::span<const tetra::Triangle> patch_arena) {
  release_active_slots();
  ++metrics_.global_compactions;
  std::size_t triangle_count{};
  for(const auto& patch:patches)triangle_count+=patch.triangle_count;
  const auto chunk_count=required_chunks(patches);
  chunks_.reserve(std::max(chunks_.capacity(),chunk_count));
  segments_.reserve(std::max(segments_.capacity(),patches.size()+chunk_count));

  const auto begin_chunk=[&]{
    SurfaceDrawChunkRecord chunk;
    chunk.arena_slot=allocate_slot();
    chunk.segment_begin=segments_.size();
    chunk.content_revision=next_content_revision_++;
    if(next_content_revision_==0U)next_content_revision_=1U;
    chunks_.push_back(chunk);
  };
  bool previous_large_patch{};
  for(const auto& patch:patches){
    if(patch.triangle_count==0U)continue;
    ++metrics_.nonempty_patches;
    const bool large_patch=is_large_patch(patch.triangle_count);
    if((large_patch||previous_large_patch)&&!chunks_.empty()&&
       chunks_.back().triangle_count!=chunk_capacity_)
      begin_chunk();
    std::size_t source_offset{};
    bool first_segment=true;
    while(source_offset<patch.triangle_count){
      if(chunks_.empty()||chunks_.back().triangle_count==chunk_capacity_)
        begin_chunk();
      auto& chunk=chunks_.back();
      if(first_segment&&chunk.triangle_count!=0U)++metrics_.chunk_merges;
      const auto count=std::min(
          patch.triangle_count-source_offset,
          chunk_capacity_-chunk.triangle_count);
      const auto destination=chunk.arena_slot*chunk_capacity_+chunk.triangle_count;
      std::copy_n(
          patch_arena.begin()+static_cast<std::ptrdiff_t>(
              patch.triangle_begin+source_offset),
          count,arena_.begin()+static_cast<std::ptrdiff_t>(destination));
      segments_.push_back({
          patch.logical_owner,source_offset,chunks_.size()-1U,destination,count});
      ++chunk.segment_count;
      chunk.triangle_count+=count;
      source_offset+=count;
      if(source_offset<patch.triangle_count)++metrics_.chunk_splits;
      first_segment=false;
    }
    previous_large_patch=large_patch;
  }
  metrics_.copied_bytes=triangle_count*sizeof(tetra::Triangle);
}

void SurfaceDrawChunkStorage::pack(
    std::span<const SurfacePatchRecord> patches,
    std::span<const tetra::Triangle> patch_arena) {
  const auto start=std::chrono::steady_clock::now();
  std::size_t triangle_count{};
  tetra::TetId previous_owner=tetra::invalid_tet;
  bool have_previous{};
  std::vector<RetainedPatch> next_patches;
  next_patches.reserve(patches.size());
  for(const auto& patch:patches){
    if(have_previous&&patch.logical_owner<=previous_owner)
      throw std::invalid_argument("surface patches must be strictly owner sorted");
    if(patch.triangle_begin>patch_arena.size()||
       patch.triangle_count>patch_arena.size()-patch.triangle_begin)
      throw std::out_of_range("surface patch triangle range exceeds its arena");
    if(patch.triangle_count>
       std::numeric_limits<std::size_t>::max()-triangle_count)
      throw std::overflow_error("surface draw triangle count overflow");
    triangle_count+=patch.triangle_count;
    next_patches.push_back({patch.logical_owner,patch.field_revision,
                            patch.topology_hash,patch.triangle_count});
    previous_owner=patch.logical_owner;
    have_previous=true;
  }

  metrics_={};
  metrics_.strategy=strategy_;
  metrics_.chunk_capacity=chunk_capacity_;
  metrics_.large_patch_threshold=large_patch_threshold_;
  metrics_.source_patches=patches.size();
  metrics_.nonempty_patches=static_cast<std::size_t>(std::ranges::count_if(
      patches,[](const auto& patch){return patch.triangle_count!=0U;}));
  for(const auto& patch:patches){
    if(!is_large_patch(patch.triangle_count))continue;
    ++metrics_.large_patches;
    metrics_.large_patch_triangles+=patch.triangle_count;
  }
  const auto same_patch=[](const RetainedPatch& old_patch,
                           const RetainedPatch& new_patch){
    return old_patch.logical_owner==new_patch.logical_owner&&
        old_patch.field_revision==new_patch.field_revision&&
        old_patch.topology_hash==new_patch.topology_hash&&
        old_patch.triangle_count==new_patch.triangle_count;
  };

  if(retained_patches_.empty()&&chunks_.empty()){
    compact(patches,patch_arena);
    retained_patches_=std::move(next_patches);
    finish_metrics(triangle_count,start);
    return;
  }

  bool identical=retained_patches_.size()==next_patches.size();
  bool same_layout=identical;
  std::vector<bool> dirty(next_patches.size(),false);
  if(identical){
    for(std::size_t index=0;index<next_patches.size();++index){
      same_layout&=retained_patches_[index].logical_owner==
                       next_patches[index].logical_owner&&
          retained_patches_[index].triangle_count==next_patches[index].triangle_count;
      dirty[index]=!same_patch(retained_patches_[index],next_patches[index]);
      identical&=!dirty[index];
    }
  }
  if(identical){
    metrics_.reused_chunks=chunks_.size();
    metrics_.reused_bytes=triangle_count*sizeof(tetra::Triangle);
    retained_patches_=std::move(next_patches);
    finish_metrics(triangle_count,start);
    return;
  }

  if(same_layout){
    std::vector<bool> dirty_chunks(chunks_.size(),false);
    for(std::size_t patch_index=0;patch_index<patches.size();++patch_index){
      if(!dirty[patch_index])continue;
      ++metrics_.dirty_patches;
      const auto& patch=patches[patch_index];
      for(const auto& segment:segments_){
        if(segment.logical_owner!=patch.logical_owner)continue;
        std::copy_n(
            patch_arena.begin()+static_cast<std::ptrdiff_t>(
                patch.triangle_begin+segment.source_triangle_offset),
            segment.triangle_count,
            arena_.begin()+static_cast<std::ptrdiff_t>(segment.triangle_begin));
        dirty_chunks[segment.chunk_index]=true;
        metrics_.copied_bytes+=segment.triangle_count*sizeof(tetra::Triangle);
      }
    }
    metrics_.dirty_chunks=static_cast<std::size_t>(
        std::ranges::count(dirty_chunks,true));
    for(std::size_t index=0;index<dirty_chunks.size();++index){
      if(!dirty_chunks[index])continue;
      chunks_[index].content_revision=next_content_revision_++;
      if(next_content_revision_==0U)next_content_revision_=1U;
    }
    metrics_.reused_chunks=chunks_.size()-metrics_.dirty_chunks;
    metrics_.reused_bytes=(triangle_count-
        metrics_.copied_bytes/sizeof(tetra::Triangle))*sizeof(tetra::Triangle);
    ++metrics_.local_repacks;
    retained_patches_=std::move(next_patches);
    finish_metrics(triangle_count,start);
    return;
  }

  tetra::TetId low=std::numeric_limits<tetra::TetId>::max();
  tetra::TetId high{};
  std::size_t old_index{},new_index{};
  while(old_index<retained_patches_.size()||new_index<next_patches.size()){
    if(new_index==next_patches.size()||
       (old_index<retained_patches_.size()&&
        retained_patches_[old_index].logical_owner<next_patches[new_index].logical_owner)){
      low=std::min(low,retained_patches_[old_index].logical_owner);
      high=std::max(high,retained_patches_[old_index].logical_owner);
      ++metrics_.dirty_patches;
      ++old_index;
    }else if(old_index==retained_patches_.size()||
             next_patches[new_index].logical_owner<
                 retained_patches_[old_index].logical_owner){
      low=std::min(low,next_patches[new_index].logical_owner);
      high=std::max(high,next_patches[new_index].logical_owner);
      ++metrics_.dirty_patches;
      ++new_index;
    }else{
      if(!same_patch(retained_patches_[old_index],next_patches[new_index])){
        low=std::min(low,next_patches[new_index].logical_owner);
        high=std::max(high,next_patches[new_index].logical_owner);
        ++metrics_.dirty_patches;
      }
      ++old_index;
      ++new_index;
    }
  }

  constexpr std::size_t local_chunk_budget=64U;
  if(chunks_.empty()||low==std::numeric_limits<tetra::TetId>::max()){
    compact(patches,patch_arena);
    retained_patches_=std::move(next_patches);
    finish_metrics(triangle_count,start);
    return;
  }

  const auto chunk_first_owner=[&](std::size_t chunk_index){
    return segments_[chunks_[chunk_index].segment_begin].logical_owner;
  };
  const auto chunk_last_owner=[&](std::size_t chunk_index){
    const auto& chunk=chunks_[chunk_index];
    return segments_[chunk.segment_begin+chunk.segment_count-1U].logical_owner;
  };
  std::size_t first_chunk=0U;
  while(first_chunk+1U<chunks_.size()&&chunk_last_owner(first_chunk)<low)
    ++first_chunk;
  std::size_t last_chunk=first_chunk;
  while(last_chunk+1U<chunks_.size()&&chunk_first_owner(last_chunk+1U)<=high)
    ++last_chunk;
  if(first_chunk>0U)--first_chunk;
  if(last_chunk+1U<chunks_.size())++last_chunk;
  while(first_chunk>0U&&
        chunk_last_owner(first_chunk-1U)==chunk_first_owner(first_chunk))
    --first_chunk;
  while(last_chunk+1U<chunks_.size()&&
        chunk_first_owner(last_chunk+1U)==chunk_last_owner(last_chunk))
    ++last_chunk;
  low=std::min(low,chunk_first_owner(first_chunk));
  high=std::max(high,chunk_last_owner(last_chunk));

  const auto patch_begin=static_cast<std::size_t>(std::distance(
      next_patches.begin(),std::lower_bound(
          next_patches.begin(),next_patches.end(),low,
          [](const auto& patch,tetra::TetId owner){
            return patch.logical_owner<owner;
          })));
  const auto patch_end=static_cast<std::size_t>(std::distance(
      next_patches.begin(),std::upper_bound(
          next_patches.begin(),next_patches.end(),high,
          [](tetra::TetId owner,const auto& patch){
            return owner<patch.logical_owner;
          })));
  std::size_t local_triangles{};
  for(std::size_t index=patch_begin;index<patch_end;++index)
    local_triangles+=patches[index].triangle_count;
  const auto replacement_chunk_count=required_chunks(
      patches.subspan(patch_begin,patch_end-patch_begin));
  const auto replaced_chunks=last_chunk-first_chunk+1U;
  if(replaced_chunks>local_chunk_budget||
     replacement_chunk_count>local_chunk_budget||
     metrics_.dirty_patches*2U>std::max(retained_patches_.size(),next_patches.size())){
    compact(patches,patch_arena);
    retained_patches_=std::move(next_patches);
    finish_metrics(triangle_count,start);
    return;
  }

  std::vector<std::size_t> local_slots;
  local_slots.reserve(replacement_chunk_count);
  for(std::size_t index=first_chunk;
      index<=last_chunk&&local_slots.size()<replacement_chunk_count;++index)
    local_slots.push_back(chunks_[index].arena_slot);
  while(local_slots.size()<replacement_chunk_count)
    local_slots.push_back(allocate_slot());
  for(std::size_t index=first_chunk+
          std::min(replacement_chunk_count,replaced_chunks);
      index<=last_chunk;++index){
    free_ranges_.push_back({chunks_[index].arena_slot,1U});
    ++metrics_.released_slots;
  }
  normalize_free_ranges();

  std::vector<SurfaceDrawChunkRecord> replacement_chunks;
  std::vector<SurfaceDrawPatchSegment> replacement_segments;
  replacement_chunks.reserve(replacement_chunk_count);
  replacement_segments.reserve(
      (patch_end-patch_begin)+replacement_chunk_count);
  const auto begin_replacement_chunk=[&]{
    SurfaceDrawChunkRecord chunk;
    chunk.arena_slot=local_slots[replacement_chunks.size()];
    chunk.segment_begin=replacement_segments.size();
    chunk.content_revision=next_content_revision_++;
    if(next_content_revision_==0U)next_content_revision_=1U;
    replacement_chunks.push_back(chunk);
  };
  bool previous_large_patch{};
  for(std::size_t patch_index=patch_begin;patch_index<patch_end;++patch_index){
    const auto& patch=patches[patch_index];
    if(patch.triangle_count==0U)continue;
    const bool large_patch=is_large_patch(patch.triangle_count);
    if((large_patch||previous_large_patch)&&!replacement_chunks.empty()&&
       replacement_chunks.back().triangle_count!=chunk_capacity_)
      begin_replacement_chunk();
    std::size_t source_offset{};
    while(source_offset<patch.triangle_count){
      if(replacement_chunks.empty()||
         replacement_chunks.back().triangle_count==chunk_capacity_)
        begin_replacement_chunk();
      auto& chunk=replacement_chunks.back();
      const auto count=std::min(
          patch.triangle_count-source_offset,
          chunk_capacity_-chunk.triangle_count);
      const auto destination=chunk.arena_slot*chunk_capacity_+chunk.triangle_count;
      std::copy_n(
          patch_arena.begin()+static_cast<std::ptrdiff_t>(
              patch.triangle_begin+source_offset),count,
          arena_.begin()+static_cast<std::ptrdiff_t>(destination));
      replacement_segments.push_back({patch.logical_owner,source_offset,
          replacement_chunks.size()-1U,destination,count});
      ++chunk.segment_count;
      chunk.triangle_count+=count;
      source_offset+=count;
    }
    previous_large_patch=large_patch;
  }

  std::vector<SurfaceDrawChunkRecord> next_chunks;
  std::vector<SurfaceDrawPatchSegment> next_segments;
  next_chunks.reserve(chunks_.size()-replaced_chunks+replacement_chunks.size());
  next_segments.reserve(segments_.size()+replacement_segments.size());
  const auto append_old_chunk=[&](std::size_t old_chunk_index){
    auto chunk=chunks_[old_chunk_index];
    const auto new_chunk_index=next_chunks.size();
    const auto old_segment_begin=chunk.segment_begin;
    chunk.segment_begin=next_segments.size();
    next_chunks.push_back(chunk);
    for(std::size_t offset=0;offset<chunk.segment_count;++offset){
      auto segment=segments_[old_segment_begin+offset];
      segment.chunk_index=new_chunk_index;
      next_segments.push_back(segment);
    }
  };
  for(std::size_t index=0;index<first_chunk;++index)append_old_chunk(index);
  for(std::size_t index=0;index<replacement_chunks.size();++index){
    auto chunk=replacement_chunks[index];
    const auto new_chunk_index=next_chunks.size();
    const auto replacement_begin=chunk.segment_begin;
    chunk.segment_begin=next_segments.size();
    next_chunks.push_back(chunk);
    for(std::size_t offset=0;offset<chunk.segment_count;++offset){
      auto segment=replacement_segments[replacement_begin+offset];
      segment.chunk_index=new_chunk_index;
      next_segments.push_back(segment);
    }
  }
  for(std::size_t index=last_chunk+1U;index<chunks_.size();++index)
    append_old_chunk(index);
  chunks_=std::move(next_chunks);
  segments_=std::move(next_segments);

  metrics_.dirty_chunks=replaced_chunks;
  metrics_.reused_chunks=chunks_.size()-replacement_chunks.size();
  metrics_.reused_bytes=(triangle_count-local_triangles)*sizeof(tetra::Triangle);
  metrics_.copied_bytes=local_triangles*sizeof(tetra::Triangle);
  metrics_.local_repacks=1U;
  metrics_.overflow_splits=replacement_chunk_count>replaced_chunks?
      replacement_chunk_count-replaced_chunks:0U;
  metrics_.underfull_merges=replaced_chunks>replacement_chunk_count?
      replaced_chunks-replacement_chunk_count:0U;
  retained_patches_=std::move(next_patches);
  finish_metrics(triangle_count,start);
}

std::vector<tetra::Triangle> direct_pack_surface_patches(
    std::span<const SurfacePatchRecord> patches,
    std::span<const tetra::Triangle> patch_arena) {
  std::size_t triangle_count{};
  for(const auto& patch:patches){
    if(patch.triangle_begin>patch_arena.size()||
       patch.triangle_count>patch_arena.size()-patch.triangle_begin)
      throw std::out_of_range("surface patch triangle range exceeds its arena");
    if(patch.triangle_count>
       std::numeric_limits<std::size_t>::max()-triangle_count)
      throw std::overflow_error("surface draw triangle count overflow");
    triangle_count+=patch.triangle_count;
  }
  std::vector<tetra::Triangle> packed;
  packed.reserve(triangle_count);
  for(const auto& patch:patches)
    packed.insert(
        packed.end(),
        patch_arena.begin()+static_cast<std::ptrdiff_t>(patch.triangle_begin),
        patch_arena.begin()+static_cast<std::ptrdiff_t>(
            patch.triangle_begin+patch.triangle_count));
  return packed;
}

std::vector<tetra::Triangle> assemble_surface_draw_chunks(
    const SurfaceDrawChunkStorage& storage) {
  std::vector<tetra::Triangle> packed;
  packed.reserve(storage.metrics().triangles);
  for(const auto& chunk:storage.chunks()){
    if(chunk.arena_slot>=storage.metrics().retained_slots||
       chunk.triangle_count>storage.chunk_capacity())
      throw std::out_of_range("surface draw chunk exceeds its arena slot");
    const auto begin=chunk.arena_slot*storage.chunk_capacity();
    if(begin>storage.arena().size()||
       chunk.triangle_count>storage.arena().size()-begin)
      throw std::out_of_range("surface draw chunk range exceeds its arena");
    packed.insert(
        packed.end(),
        storage.arena().begin()+static_cast<std::ptrdiff_t>(begin),
        storage.arena().begin()+static_cast<std::ptrdiff_t>(
            begin+chunk.triangle_count));
  }
  return packed;
}

SurfaceHostStagingStorage::SurfaceHostStagingStorage(
    std::size_t triangle_chunk_capacity)
    :triangle_chunk_capacity_(triangle_chunk_capacity) {
  if(triangle_chunk_capacity_==0U||
     triangle_chunk_capacity_>std::numeric_limits<std::size_t>::max()/3U)
    throw std::invalid_argument(
        "surface host staging triangle capacity must be positive and bounded");
}

void SurfaceHostStagingStorage::normalize_free_ranges(
    std::vector<SurfaceHostFreeRange>& ranges) {
  std::sort(ranges.begin(),ranges.end(),[](const auto& left,const auto& right){
    return left.begin_slot<right.begin_slot;
  });
  std::size_t output{};
  for(const auto range:ranges){
    if(range.slot_count==0U)continue;
    if(output!=0U&&ranges[output-1U].begin_slot+
           ranges[output-1U].slot_count>=range.begin_slot){
      auto& previous=ranges[output-1U];
      previous.slot_count=std::max(
          previous.begin_slot+previous.slot_count,
          range.begin_slot+range.slot_count)-previous.begin_slot;
    }else ranges[output++]=range;
  }
  ranges.resize(output);
}

std::size_t SurfaceHostStagingStorage::allocate_slot(
    std::vector<SurfaceHostFreeRange>& candidate_free_ranges,
    SurfaceHostStagingMetrics& candidate_metrics) {
  if(!candidate_free_ranges.empty()){
    const auto slot=candidate_free_ranges.front().begin_slot++;
    if(--candidate_free_ranges.front().slot_count==0U)
      candidate_free_ranges.erase(candidate_free_ranges.begin());
    ++candidate_metrics.reused_slots;
    return slot;
  }
  const auto slot=arena_.size()/vertex_slot_capacity();
  if(slot==std::numeric_limits<std::size_t>::max()||
     vertex_slot_capacity()>std::numeric_limits<std::size_t>::max()-arena_.size())
    throw std::overflow_error("surface host staging arena size overflow");
  arena_.resize(arena_.size()+vertex_slot_capacity());
  ++candidate_metrics.allocated_slots;
  return slot;
}

void SurfaceHostStagingStorage::stage(
    const SurfaceDrawChunkStorage& source,
    std::span<const SceneVertex> logical_vertices) {
  const auto start=std::chrono::steady_clock::now();
  if(source.chunk_capacity()!=triangle_chunk_capacity_)
    throw std::invalid_argument(
        "surface host staging capacity differs from its source chunks");
  if(source.metrics().triangles>
     std::numeric_limits<std::size_t>::max()/3U||
     logical_vertices.size()!=source.metrics().triangles*3U)
    throw std::invalid_argument(
        "surface host staging vertices do not cover the source triangles");
  std::vector<bool> source_slots(source.metrics().retained_slots,false);
  std::size_t logical_vertex_offset{};
  for(const auto& chunk:source.chunks()){
    if(chunk.arena_slot>=source_slots.size()||source_slots[chunk.arena_slot]||
       chunk.triangle_count==0U||
       chunk.triangle_count>triangle_chunk_capacity_||
       chunk.content_revision==0U)
      throw std::invalid_argument(
          "surface host staging source chunk table is invalid");
    source_slots[chunk.arena_slot]=true;
    const auto vertex_count=chunk.triangle_count*3U;
    if(vertex_count>logical_vertices.size()-logical_vertex_offset)
      throw std::invalid_argument(
          "surface host staging source order exceeds its vertices");
    logical_vertex_offset+=vertex_count;
  }
  if(logical_vertex_offset!=logical_vertices.size())
    throw std::invalid_argument(
        "surface host staging source order leaves unmatched vertices");

  SurfaceHostStagingMetrics candidate_metrics;
  candidate_metrics.publication_generation=metrics_.publication_generation+1U;
  if(candidate_metrics.publication_generation==0U)
    candidate_metrics.publication_generation=1U;
  candidate_metrics.source_chunks=source.chunks().size();
  std::vector<SurfaceHostFreeRange> candidate_free_ranges=free_ranges_;
  std::vector<bool> retained_ranges(ranges_.size(),false);
  range_scratch_.clear();
  range_scratch_.reserve(std::max(range_scratch_.capacity(),source.chunks().size()));
  logical_vertex_offset=0U;
  for(const auto& chunk:source.chunks()){
    const auto vertex_count=chunk.triangle_count*3U;
    const auto source_vertices=logical_vertices.subspan(
        logical_vertex_offset,vertex_count);
    std::size_t retained_index=ranges_.size();
    for(std::size_t index=0;index<ranges_.size();++index){
      const auto& range=ranges_[index];
      if(range.source_arena_slot==chunk.arena_slot&&
         range.source_content_revision==chunk.content_revision&&
         range.triangle_vertex_count==vertex_count&&
         (source_vertices.empty()||std::memcmp(
             source_vertices.data(),
             arena_.data()+static_cast<std::ptrdiff_t>(
                 range.triangle_vertex_begin),
             source_vertices.size_bytes())==0)){
        retained_index=index;
        break;
      }
    }
    std::size_t host_slot{};
    std::uint64_t host_content_revision{};
    if(retained_index!=ranges_.size()){
      retained_ranges[retained_index]=true;
      host_slot=ranges_[retained_index].host_slot;
      host_content_revision=ranges_[retained_index].host_content_revision;
      ++candidate_metrics.reused_ranges;
    }else{
      host_slot=allocate_slot(candidate_free_ranges,candidate_metrics);
      host_content_revision=next_content_revision_++;
      if(next_content_revision_==0U)next_content_revision_=1U;
      const auto destination=host_slot*vertex_slot_capacity();
      std::copy_n(
          source_vertices.begin(),
          vertex_count,
          arena_.begin()+static_cast<std::ptrdiff_t>(destination));
      ++candidate_metrics.dirty_ranges;
      candidate_metrics.staged_triangle_bytes+=
          vertex_count*sizeof(SceneVertex);
      candidate_metrics.aliased_wire_bytes+=
          vertex_count*sizeof(SceneVertex);
    }
    const auto begin=host_slot*vertex_slot_capacity();
    range_scratch_.push_back({
        .host_slot=host_slot,
        .source_arena_slot=chunk.arena_slot,
        .source_content_revision=chunk.content_revision,
        .host_content_revision=host_content_revision,
        .triangle_vertex_begin=begin,
        .triangle_vertex_count=vertex_count,
        .wire_vertex_begin=begin,
        .wire_vertex_count=vertex_count});
    logical_vertex_offset+=vertex_count;
  }

  for(std::size_t index=0;index<ranges_.size();++index){
    if(retained_ranges[index])continue;
    candidate_free_ranges.push_back({ranges_[index].host_slot,1U});
    ++candidate_metrics.released_slots;
  }
  normalize_free_ranges(candidate_free_ranges);
  ranges_.swap(range_scratch_);
  free_ranges_.swap(candidate_free_ranges);
  candidate_metrics.active_ranges=ranges_.size();
  candidate_metrics.retained_slots=arena_.size()/vertex_slot_capacity();
  for(const auto range:free_ranges_)
    candidate_metrics.free_slots+=range.slot_count;
  candidate_metrics.retained_bytes=
      ranges_.capacity()*sizeof(SurfaceHostDrawRange)+
      range_scratch_.capacity()*sizeof(SurfaceHostDrawRange)+
      free_ranges_.capacity()*sizeof(SurfaceHostFreeRange)+
      arena_.capacity()*sizeof(SceneVertex);
  candidate_metrics.stage_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-start).count();
  metrics_=candidate_metrics;
}

std::vector<SceneVertex> assemble_surface_host_staging(
    const SurfaceHostStagingStorage& storage) {
  std::size_t vertex_count{};
  for(const auto& range:storage.ranges()){
    if(range.host_slot>=storage.metrics().retained_slots||
       range.triangle_vertex_count>storage.vertex_slot_capacity()||
       range.triangle_vertex_begin!=
           range.host_slot*storage.vertex_slot_capacity()||
       range.wire_vertex_begin!=range.triangle_vertex_begin||
       range.wire_vertex_count!=range.triangle_vertex_count||
       range.triangle_vertex_count>
           std::numeric_limits<std::size_t>::max()-vertex_count)
      throw std::out_of_range("surface host staging range is invalid");
    vertex_count+=range.triangle_vertex_count;
  }
  std::vector<SceneVertex> assembled;
  assembled.reserve(vertex_count);
  for(const auto& range:storage.ranges()){
    if(range.triangle_vertex_begin>storage.arena().size()||
       range.triangle_vertex_count>
           storage.arena().size()-range.triangle_vertex_begin)
      throw std::out_of_range("surface host staging range exceeds its arena");
    assembled.insert(
        assembled.end(),
        storage.arena().begin()+static_cast<std::ptrdiff_t>(
            range.triangle_vertex_begin),
        storage.arena().begin()+static_cast<std::ptrdiff_t>(
            range.triangle_vertex_begin+range.triangle_vertex_count));
  }
  return assembled;
}

void SurfaceDeviceUploadPlanner::prepare(
    const SurfaceHostStagingStorage& host,
    std::size_t device_vertex_capacity) {
  metrics_={};
  metrics_.prepared=true;
  metrics_.source_generation=host.metrics().publication_generation;
  if(metrics_.source_generation==0U)
    throw std::invalid_argument(
        "surface device upload requires a published host generation");
  if(host.metrics().retained_slots>
     std::numeric_limits<std::size_t>::max()/host.vertex_slot_capacity())
    throw std::overflow_error("surface device vertex capacity overflow");
  metrics_.required_vertex_capacity=
      host.metrics().retained_slots*host.vertex_slot_capacity();
  metrics_.full_reallocation=
      metrics_.required_vertex_capacity>device_vertex_capacity;
  const auto slot_count=host.metrics().retained_slots;
  slot_scratch_.assign(slot_count,{});
  std::copy_n(published_slots_.begin(),
              std::min(published_slots_.size(),slot_scratch_.size()),
              slot_scratch_.begin());
  std::vector<bool> active_slots(slot_count,false);
  upload_scratch_.clear();
  draw_scratch_.clear();
  upload_scratch_.reserve(
      std::max(upload_scratch_.capacity(),host.ranges().size()));
  draw_scratch_.reserve(
      std::max(draw_scratch_.capacity(),host.ranges().size()));
  for(const auto& range:host.ranges()){
    if(range.host_slot>=slot_count||active_slots[range.host_slot]||
       range.host_content_revision==0U||
       range.triangle_vertex_begin!=
           range.host_slot*host.vertex_slot_capacity()||
       range.triangle_vertex_count>host.vertex_slot_capacity()||
       range.triangle_vertex_begin>host.arena().size()||
       range.triangle_vertex_count>
           host.arena().size()-range.triangle_vertex_begin||
       range.triangle_vertex_begin>
           std::numeric_limits<std::uint32_t>::max()||
       range.triangle_vertex_count>
           std::numeric_limits<std::uint32_t>::max()||
       range.wire_vertex_begin!=range.triangle_vertex_begin||
       range.wire_vertex_count!=range.triangle_vertex_count)
      throw std::invalid_argument(
          "surface device upload host range table is invalid");
    active_slots[range.host_slot]=true;
    const SlotKey key{range.host_content_revision,
                      range.triangle_vertex_count};
    const bool dirty=metrics_.full_reallocation||
        range.host_slot>=published_slots_.size()||
        published_slots_[range.host_slot]!=key;
    if(dirty){
      upload_scratch_.push_back({
          range.triangle_vertex_begin,range.triangle_vertex_begin,
          range.triangle_vertex_count});
      metrics_.uploaded_bytes+=
          range.triangle_vertex_count*sizeof(SceneVertex);
    }else ++metrics_.reused_ranges;
    slot_scratch_[range.host_slot]=key;
    draw_scratch_.push_back({range.triangle_vertex_begin,
                             range.triangle_vertex_count});
  }
  for(std::size_t slot=0;slot<slot_scratch_.size();++slot)
    if(!active_slots[slot])slot_scratch_[slot]={};
  metrics_.upload_ranges=upload_scratch_.size();
  metrics_.draw_calls=draw_scratch_.size();
}

void SurfaceDeviceUploadPlanner::commit() {
  if(!metrics_.prepared)
    throw std::logic_error("surface device upload has no prepared publication");
  published_slots_.swap(slot_scratch_);
  published_draws_.swap(draw_scratch_);
  published_generation_=metrics_.source_generation;
  metrics_.prepared=false;
}

void SurfaceDeviceUploadPlanner::cancel() noexcept {
  slot_scratch_.clear();
  upload_scratch_.clear();
  draw_scratch_.clear();
  metrics_={};
}

void SurfaceDeviceUploadPlanner::reset() noexcept {
  cancel();
  published_slots_.clear();
  published_draws_.clear();
  published_generation_=0U;
}

void apply_surface_device_upload_plan(
    SurfaceDeviceUploadPlanner& planner,
    const SurfaceHostStagingStorage& host,
    std::vector<SceneVertex>& device_arena) {
  if(!planner.metrics().prepared)
    throw std::logic_error("surface device upload plan is not prepared");
  if(host.metrics().publication_generation!=planner.metrics().source_generation)
    throw std::invalid_argument(
        "surface device upload plan was superseded by a host publication");
  const auto grown_capacity=std::max({
      planner.metrics().required_vertex_capacity,
      device_arena.size()<=std::numeric_limits<std::size_t>::max()/2U
          ?device_arena.size()*2U:device_arena.size(),
      std::size_t{4096U}});
  std::vector<SceneVertex> candidate=planner.metrics().full_reallocation
      ?std::vector<SceneVertex>(grown_capacity):device_arena;
  if(candidate.size()<planner.metrics().required_vertex_capacity)
    candidate.resize(planner.metrics().required_vertex_capacity);
  for(const auto upload:planner.uploads()){
    if(upload.source_vertex_begin>host.arena().size()||
       upload.vertex_count>host.arena().size()-upload.source_vertex_begin||
       upload.destination_vertex_begin>candidate.size()||
       upload.vertex_count>candidate.size()-upload.destination_vertex_begin)
      throw std::out_of_range("surface device upload range exceeds its arena");
    std::copy_n(
        host.arena().begin()+static_cast<std::ptrdiff_t>(
            upload.source_vertex_begin),upload.vertex_count,
        candidate.begin()+static_cast<std::ptrdiff_t>(
            upload.destination_vertex_begin));
  }
  device_arena.swap(candidate);
  planner.commit();
}

std::vector<SceneVertex> assemble_surface_device_publication(
    const SurfaceDeviceUploadPlanner& planner,
    std::span<const SceneVertex> device_arena) {
  std::size_t vertex_count{};
  for(const auto draw:planner.published_draws()){
    if(draw.first_vertex>device_arena.size()||
       draw.vertex_count>device_arena.size()-draw.first_vertex||
       draw.vertex_count>
           std::numeric_limits<std::size_t>::max()-vertex_count)
      throw std::out_of_range("surface device draw range exceeds its arena");
    vertex_count+=draw.vertex_count;
  }
  std::vector<SceneVertex> assembled;
  assembled.reserve(vertex_count);
  for(const auto draw:planner.published_draws())
    assembled.insert(
        assembled.end(),
        device_arena.begin()+static_cast<std::ptrdiff_t>(draw.first_vertex),
        device_arena.begin()+static_cast<std::ptrdiff_t>(
            draw.first_vertex+draw.vertex_count));
  return assembled;
}

void expand_line_segments_for_upload(
    std::span<const SceneVertex> line_vertices,
    std::vector<SceneVertex>& ribbons) {
  ribbons.clear();
  ribbons.reserve((line_vertices.size()/2)*6);
  constexpr std::array<std::array<float,2>,6> corners{{
      {{0.0F,-1.0F}},{{1.0F,-1.0F}},{{1.0F,1.0F}},
      {{0.0F,-1.0F}},{{1.0F,1.0F}},{{0.0F,1.0F}}}};
  for(std::size_t line=0;line+1<line_vertices.size();line+=2){
    for(const auto corner:corners){
      SceneVertex vertex{};
      std::copy_n(line_vertices[line].position,3,vertex.position);
      std::copy_n(line_vertices[line+1].position,3,vertex.normal);
      std::copy_n(line_vertices[line].colour,3,vertex.colour);
      vertex.diagnostics[0]=corner[0];
      vertex.diagnostics[1]=corner[1];
      ribbons.push_back(vertex);
    }
  }
}

PreparedScene prepare_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere,
                            SurfaceMethod surface_method, MaterialRule material_rule,
                            bool show_faces, bool show_hierarchy_edges,
                            bool show_surface_edges, bool depth_colours,
                            bool show_volume_edges, bool show_volume_faces,
                            double x_cut_position,
                            VolumeConnectionMethod volume_connection_method,
                            StencilConstruction stencil_construction,
                            StencilSelectionObjective stencil_selection_objective,
                            ScenePreparationOptions preparation,
                            std::span<const tetra::Triangle> surface_override,
                            bool surface_override_is_owner_patches) {
  PreparedScene scene;
  const auto leaves = mesh.conforming_volume().addresses();
  scene.relations.reserve(leaves.size());
  scene.triangle_vertices.reserve((show_faces || show_surface_edges) ? leaves.size() * 12 : 0);
  scene.hierarchy_line_vertices.reserve(show_hierarchy_edges ? leaves.size() * 12 : 0);
  std::vector<PackedEdge> hierarchy_edges;
  if (show_hierarchy_edges) hierarchy_edges.reserve(leaves.size() * 6);
  std::vector<MaterialFace> material_faces;
  if (show_faces || show_surface_edges || show_volume_edges || show_volume_faces)
    material_faces.reserve(leaves.size() * 4);
  std::vector<tetra::TetId> material_tetrahedra;
  std::vector<tetra::TetId> boundary_tetrahedra;
  std::vector<std::array<tetra::VertexId,3>> material_boundary_faces;
  if (show_volume_faces || show_volume_edges) material_tetrahedra.reserve(leaves.size());
  const bool connected_volume=uses_connected_volume(volume_connection_method);
  const bool need_material_selection=surface_method==SurfaceMethod::full_tetrahedra||
      ((show_volume_faces||show_volume_edges)&&!connected_volume);
  const bool need_material_faces =
      (surface_method == SurfaceMethod::full_tetrahedra && (show_faces || show_surface_edges)) ||
      ((show_volume_edges||show_volume_faces)&&!connected_volume);

  constexpr std::array<std::array<std::size_t, 3>, 4> faces{{{{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
  const auto add_triangle = [&scene](tetra::Vec3 point, std::array<float, 3> colour, tetra::Vec3 normal) {
    scene.triangle_vertices.push_back({{static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)},
                                       {colour[0], colour[1], colour[2]},
                                       {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)}});
  };

  const auto statistics_start = std::chrono::steady_clock::now();
  for (const tetra::TetId id : leaves) {
    if(preparation.summary_statistics){
      const auto depth = static_cast<std::size_t>(mesh.refinement_depth(id));
      if (scene.depth_counts.size() <= depth) scene.depth_counts.resize(depth + 1);
      ++scene.depth_counts[depth];
      scene.total_volume += mesh.signed_volume(id);
    }

    const auto relation = tetra::classify_tetrahedron(mesh, id, sphere);
    scene.relations.push_back(relation);
    if(preparation.summary_statistics){
      if (relation == tetra::SurfaceRelation::inside) ++scene.inside_count;
      else if (relation == tetra::SurfaceRelation::outside) ++scene.outside_count;
      else ++scene.intersecting_count;
    }
  }
  scene.summary_statistics_available=preparation.summary_statistics;
  scene.statistics_milliseconds = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - statistics_start).count();

  const auto whole_cell_cut=need_material_selection&&is_variational_material_rule(material_rule)
      ? tetra::build_whole_cell_cut(mesh,sphere,whole_cell_options(material_rule)) : tetra::WholeCellCut{};
  if(need_material_selection&&is_variational_material_rule(material_rule)){
    scene.whole_cell_boundary_faces=whole_cell_cut.boundary_faces.size();
    scene.whole_cell_nonmanifold_edges=whole_cell_cut.nonmanifold_boundary_edges;
    scene.whole_cell_selected_volume=whole_cell_cut.selected_volume;
    scene.whole_cell_solve_milliseconds=whole_cell_cut.solve_milliseconds;
    scene.whole_cell_hash=whole_cell_cut.hash;
  }

  const auto geometry_start = std::chrono::steady_clock::now();
  for (std::size_t leaf_index = 0; leaf_index < leaves.size(); ++leaf_index) {
    const tetra::TetId id = leaves[leaf_index];
    const auto& tet = mesh.tetrahedron(id).vertices;
    const bool material = need_material_selection&&
        (is_variational_material_rule(material_rule)
            ? whole_cell_cut.selected(leaf_index)
            : is_material(mesh,id,sphere,scene.relations[leaf_index],material_rule));
    if ((show_volume_faces || show_volume_edges) && !connected_volume && material)
      material_tetrahedra.push_back(id);
    const bool selected = surface_method == SurfaceMethod::full_tetrahedra && material;
    if (selected) ++scene.selected_count;
    if (need_material_faces && material && !is_variational_material_rule(material_rule)) {
      for (const auto& face : faces) {
        std::array<tetra::VertexId, 3> vertices{{tet[face[0]], tet[face[1]], tet[face[2]]}};
        auto key = vertices;
        std::sort(key.begin(), key.end());
        material_faces.push_back({key, vertices, id});
      }
    }
    if (show_hierarchy_edges) {
      for (std::size_t first = 0; first < 4; ++first)
        for (std::size_t second = first + 1; second < 4; ++second)
          hierarchy_edges.push_back(pack_edge(tet[first], tet[second]));
    }
  }

  if(need_material_faces&&is_variational_material_rule(material_rule)){
    material_faces.reserve(whole_cell_cut.boundary_faces.size());
    for(const auto& face:whole_cell_cut.boundary_faces){
      auto key=face.vertices;std::sort(key.begin(),key.end());
      material_faces.push_back({key,face.vertices,leaves[face.inside_leaf]});
    }
  }

  if (need_material_faces) {
    std::sort(material_faces.begin(), material_faces.end(), [](const MaterialFace& first, const MaterialFace& second) {
      return first.key < second.key;
    });
    for (std::size_t begin = 0; begin < material_faces.size();) {
      std::size_t end = begin + 1;
      while (end < material_faces.size() && material_faces[end].key == material_faces[begin].key) ++end;
      // A face occurring twice is internal to the selected full-tetrahedron
      // material volume. Submit only the exposed boundary of that union.
      if (end - begin == 1) {
        const auto& material_face = material_faces[begin];
        if (show_volume_faces || show_volume_edges) {
          boundary_tetrahedra.push_back(material_face.tetrahedron);
          material_boundary_faces.push_back(material_face.key);
        }
        if (surface_method != SurfaceMethod::full_tetrahedra || (!show_faces && !show_surface_edges)) {
          begin = end;
          continue;
        }
        const auto& tet = mesh.tetrahedron(material_face.tetrahedron).vertices;
        tetra::Vec3 tet_centre{};
        for (const auto vertex : tet) tet_centre = tet_centre + mesh.vertices()[vertex];
        tet_centre = tet_centre / 4.0;
        const tetra::Vec3 a = mesh.vertices()[material_face.vertices[0]];
        const tetra::Vec3 b = mesh.vertices()[material_face.vertices[1]];
        const tetra::Vec3 c = mesh.vertices()[material_face.vertices[2]];
        auto normal = face_normal(a, b, c);
        const tetra::Vec3 centre{(a.x+b.x+c.x)/3.0, (a.y+b.y+c.y)/3.0, (a.z+b.z+c.z)/3.0};
        const tetra::Vec3 outward{centre.x-tet_centre.x, centre.y-tet_centre.y, centre.z-tet_centre.z};
        if (normal.x*outward.x + normal.y*outward.y + normal.z*outward.z < 0.0)
          normal = {-normal.x, -normal.y, -normal.z};
        add_triangle(a, {0.25F, 0.58F, 0.90F}, normal);
        add_triangle(b, {0.25F, 0.58F, 0.90F}, normal);
        add_triangle(c, {0.25F, 0.58F, 0.90F}, normal);
      }
      begin = end;
    }
  }

  if (show_volume_faces || show_volume_edges) {
    std::sort(boundary_tetrahedra.begin(), boundary_tetrahedra.end());
    boundary_tetrahedra.erase(
        std::unique(boundary_tetrahedra.begin(), boundary_tetrahedra.end()), boundary_tetrahedra.end());
  }

  if (show_hierarchy_edges) {
    std::sort(hierarchy_edges.begin(), hierarchy_edges.end());
    hierarchy_edges.erase(std::unique(hierarchy_edges.begin(), hierarchy_edges.end()), hierarchy_edges.end());
    scene.hierarchy_line_vertices.reserve(hierarchy_edges.size() * 2);
    const float tint = depth_colours ? 0.70F : 0.64F;
    const std::array<float, 3> colour{tint, tint + 0.08F, std::min(1.0F, tint + 0.18F)};
    for (const auto packed : hierarchy_edges) {
      const auto edge = unpack_edge(packed);
      for (const auto vertex : edge) {
        const auto point = mesh.vertices()[vertex];
        scene.hierarchy_line_vertices.push_back({{static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)},
                                                 {colour[0], colour[1], colour[2]}});
      }
    }
  }
  const bool connected_optimized_surface=
      surface_method==SurfaceMethod::surface_optimization&&
      uses_connected_volume(volume_connection_method);
  if(!surface_override.empty()||surface_override_is_owner_patches){
    std::array<float,3> colour{{0.24F,0.76F,0.38F}};
    if(surface_override_is_owner_patches){
      if(surface_method==SurfaceMethod::marching_tetrahedra){
        scene.marching_tetrahedra_triangles=surface_override.size();
        colour={{0.32F,0.76F,0.42F}};
      }else if(surface_method==SurfaceMethod::lattice_cleaving){
        build_lattice_cleaved_cells(scene,mesh,sphere);
        colour={{0.80F,0.58F,0.24F}};
      }else if(surface_method==SurfaceMethod::dual_contouring){
        scene.dual_contour_triangles=surface_override.size();
        colour={{0.22F,0.72F,0.52F}};
      }else if(surface_method==SurfaceMethod::four_hexahedra){
        scene.four_hexahedra_triangles=surface_override.size();
        colour={{0.26F,0.72F,0.56F}};
      }else if(surface_method==SurfaceMethod::mixed_depth_dual){
        scene.mixed_depth_dual_triangles=surface_override.size();
        colour={{0.20F,0.74F,0.60F}};
      }
    }
    for(const auto& triangle:surface_override){
      auto normal=face_normal(triangle.a,triangle.b,triangle.c);
      if(surface_override_is_owner_patches){
        const auto centre=(triangle.a+triangle.b+triangle.c)/3.0;
        const auto outward=sphere.normal(centre);
        if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0)
          normal={-normal.x,-normal.y,-normal.z};
      }
      if(show_faces||show_surface_edges){
        add_triangle(triangle.a,colour,normal);
        add_triangle(triangle.b,colour,normal);
        add_triangle(triangle.c,colour,normal);
      }
    }
  }else if(connected_optimized_surface){
    // Build the optimized connected boundary even when the cutaway is off.
    // Turning the cutaway on must reveal cells beneath this exact surface,
    // not switch to a separately optimized representation.
    if(volume_connection_method==VolumeConnectionMethod::fixed_surface_shell)
      build_fixed_surface_shell(scene,mesh,sphere,stencil_selection_objective);
    else{
      build_adaptive_cleaved_volume(scene,mesh,sphere,volume_connection_method,
                                    stencil_construction,stencil_selection_objective);
      optimize_connected_volume_boundary(scene,sphere);
    }
    append_connected_volume(scene,mesh,sphere,std::numeric_limits<double>::infinity(),false,false,
                            show_faces,show_surface_edges);
  } else if (surface_method == SurfaceMethod::marching_tetrahedra)
    append_marching_tetrahedra(scene, mesh, sphere, show_faces, show_surface_edges);
  else if (surface_method == SurfaceMethod::lattice_cleaving)
    append_lattice_cleaving(scene, mesh, sphere, show_faces, show_surface_edges);
  else if (surface_method == SurfaceMethod::tetrahedral_layer)
    append_tetrahedral_layer(scene, mesh, sphere, show_faces, show_surface_edges);
  else if (surface_method == SurfaceMethod::dual_contouring)
    append_dual_contour(scene, mesh, sphere, show_faces, show_surface_edges);
  else if (surface_method == SurfaceMethod::four_hexahedra)
    append_four_hexahedra(scene,mesh,sphere,show_faces,show_surface_edges);
  else if (surface_method == SurfaceMethod::mixed_depth_dual)
    append_mixed_depth_dual(scene,mesh,sphere,show_faces,show_surface_edges);
  else if (surface_method == SurfaceMethod::surface_optimization)
    append_surface_optimization(scene, mesh, sphere, show_faces, show_surface_edges);
  prepare_surface_render_attributes(scene);
  if(preparation.surface_diagnostics)annotate_surface_diagnostics(scene,sphere);
  // Cut caps are diagnostic volume geometry, not part of the generated
  // isosurface metrics. Append them after surface annotation.
  if (show_volume_faces || show_volume_edges) {
    if (uses_connected_volume(volume_connection_method)) {
      if(scene.connected_volume_vertices.empty()){
        if(volume_connection_method==VolumeConnectionMethod::fixed_surface_shell)
          build_fixed_surface_shell(scene,mesh,sphere,stencil_selection_objective);
        else build_adaptive_cleaved_volume(scene,mesh,sphere,volume_connection_method,
                                           stencil_construction,stencil_selection_objective);
        if(surface_method==SurfaceMethod::surface_optimization&&
           volume_connection_method!=VolumeConnectionMethod::fixed_surface_shell)
          optimize_connected_volume_boundary(scene,sphere);
      }
      // The connected volume, not an independent display mesh, owns the
      // physical boundary in a solid cutaway. Avoid overlapping two surfaces
      // with different vertices and topology.
      const bool replace_surface=show_volume_faces;
      if(replace_surface){
        scene.triangle_vertices.clear();
        scene.visible_volume_face_triangles=0;
      }
      append_connected_volume(scene,mesh,sphere,x_cut_position,show_volume_edges,show_volume_faces,
                              replace_surface,replace_surface&&show_surface_edges);
    } else {
      append_selected_volume(scene, mesh, material_tetrahedra, boundary_tetrahedra,
                             material_boundary_faces,x_cut_position,
                             show_volume_edges,show_volume_faces,
                             surface_method==SurfaceMethod::full_tetrahedra);
    }
  }
  append_screen_space_edges(scene,show_surface_edges,show_volume_edges,
                            show_faces,show_volume_faces);
  scene.upload_preparation_milliseconds = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - geometry_start).count();
  return scene;
}

ProjectionStatistics prepare_projection_statistics(const tetra::TetMesh& mesh, const PreparedScene& scene,
                                                    const tetra::Camera& camera, double pixel_threshold) {
  ProjectionStatistics statistics;
  const auto leaves = mesh.conforming_volume().addresses();
  for (std::size_t index = 0; index < scene.relations.size(); ++index) {
    if (scene.relations[index] != tetra::SurfaceRelation::intersecting) continue;
    if (tetra::projected_tetrahedron_diameter(mesh, leaves[index], camera) > pixel_threshold)
      ++statistics.pending_count;
    else
      ++statistics.accepted_count;
  }
  return statistics;
}

void SceneCache::set_surface_patch_fallback(
    bool monolithic_fallback,bool global_fallback){
  surface_patch_metrics_={};
  surface_patch_metrics_.monolithic_fallback=monolithic_fallback;
  surface_patch_metrics_.global_fallback=global_fallback;
  surface_patch_metrics_.arena_slots=surface_patch_arena_.size();
  for(const auto range:surface_patch_free_ranges_)
    surface_patch_metrics_.free_slots+=range.count;
  surface_patch_metrics_.retained_bytes=
      surface_patch_records_.capacity()*sizeof(SurfacePatchRecord)+
      surface_patch_record_scratch_.capacity()*sizeof(SurfacePatchRecord)+
      surface_patch_arena_.capacity()*sizeof(tetra::Triangle)+
      surface_patch_output_.capacity()*sizeof(tetra::Triangle)+
      surface_patch_triangle_scratch_.capacity()*sizeof(tetra::Triangle)+
      surface_patch_free_ranges_.capacity()*sizeof(SurfacePatchFreeRange)+
      surface_patch_owner_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_topology_hash_scratch_.capacity()*sizeof(std::uint64_t)+
      surface_patch_dirty_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_incident_dirty_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_cell_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_owner_cells_.capacity()*sizeof(SurfacePatchOwnerCell)+
      dual_patch_builder_.retained_bytes()+
      four_hexahedra_patch_builder_.metrics().retained_bytes+
      mixed_depth_dual_patch_builder_.retained_bytes()+
      dual_patch_dependencies_.capacity()*
          sizeof(tetra::DualContourPatchDependency)+
      dual_patch_triangle_scratch_.capacity()*
          sizeof(tetra::DualContourPatchTriangle)+
      mixed_depth_dual_patch_dependencies_.capacity()*
          sizeof(tetra::MixedDepthDualPatchDependency)+
      mixed_depth_dual_patch_triangle_scratch_.capacity()*
          sizeof(tetra::MixedDepthDualPatchTriangle);
}

void SceneCache::update_surface_patches(
    const tetra::TetMesh& mesh,const tetra::Sphere& sphere,
    std::uint64_t field_revision,SurfaceMethod surface_method){
  const auto start=std::chrono::steady_clock::now();
  surface_patch_metrics_={};
  surface_patch_metrics_.active=true;
  const bool dual_topology=surface_method==SurfaceMethod::dual_contouring;
  const bool four_hexahedra=surface_method==SurfaceMethod::four_hexahedra;
  const bool mixed_depth_dual=surface_method==SurfaceMethod::mixed_depth_dual;

  const bool bcc=mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green;
  surface_patch_owner_scratch_.clear();
  surface_patch_owner_cells_.clear();
  if(bcc){
    const auto& owners=mesh.logical_red_owners();
    surface_patch_owner_scratch_.assign(owners.begin(),owners.end());
    const auto hashes=mesh.logical_derived_hashes();
    if(hashes.size()!=owners.size())
      throw std::logic_error("logical owner topology hashes are incomplete");
    surface_patch_topology_hash_scratch_.assign(hashes.begin(),hashes.end());
  }else{
    const auto volume=mesh.conforming_volume();
    surface_patch_owner_cells_.reserve(volume.size());
    for(std::size_t index=0;index<volume.size();++index){
      const auto cell=volume.cell(index);
      surface_patch_owner_cells_.push_back({cell.logical_owner,cell.address});
    }
    std::sort(surface_patch_owner_cells_.begin(),surface_patch_owner_cells_.end(),
              [](const auto& left,const auto& right){
                return left.owner<right.owner||
                    (left.owner==right.owner&&left.cell<right.cell);
              });
    for(const auto entry:surface_patch_owner_cells_)
      if(surface_patch_owner_scratch_.empty()||
         surface_patch_owner_scratch_.back()!=entry.owner)
        surface_patch_owner_scratch_.push_back(entry.owner);
    surface_patch_topology_hash_scratch_.clear();
    surface_patch_topology_hash_scratch_.reserve(
        surface_patch_owner_scratch_.size());
    constexpr std::uint64_t offset=1469598103934665603ULL;
    constexpr std::uint64_t prime=1099511628211ULL;
    for(const auto owner:surface_patch_owner_scratch_){
      std::uint64_t hash=offset;
      const auto begin=std::lower_bound(
          surface_patch_owner_cells_.begin(),surface_patch_owner_cells_.end(),owner,
          [](const auto& entry,tetra::TetId target){return entry.owner<target;});
      for(auto found=begin;found!=surface_patch_owner_cells_.end()&&
          found->owner==owner;++found){
        hash^=found->cell;hash*=prime;
        for(const auto vertex:mesh.tetrahedron(found->cell).vertices){
          hash^=vertex;hash*=prime;
        }
      }
      surface_patch_topology_hash_scratch_.push_back(hash);
    }
  }

  bool rebuild_all=!surface_patch_initialized_||
      surface_patch_field_revision_!=field_revision||
      surface_patch_subdivision_method_!=mesh.subdivision_method()||
      surface_patch_dual_topology_!=dual_topology||
      surface_patch_four_hexahedra_!=four_hexahedra||
      surface_patch_mixed_depth_dual_!=mixed_depth_dual||
      mesh.revision()<surface_patch_mesh_revision_;
  surface_patch_dirty_scratch_.clear();
  if(rebuild_all){
    surface_patch_dirty_scratch_=surface_patch_owner_scratch_;
  }else if(mesh.revision()!=surface_patch_mesh_revision_){
    for(std::size_t owner_index=0;
        owner_index<surface_patch_owner_scratch_.size();++owner_index){
      const auto owner=surface_patch_owner_scratch_[owner_index];
      const auto found=std::lower_bound(
          surface_patch_records_.begin(),surface_patch_records_.end(),owner,
          [](const auto& record,tetra::TetId target){
            return record.logical_owner<target;
          });
      if(found==surface_patch_records_.end()||found->logical_owner!=owner||
         found->topology_hash!=surface_patch_topology_hash_scratch_[owner_index])
        surface_patch_dirty_scratch_.push_back(owner);
    }
    for(const auto& record:surface_patch_records_)
      if(!std::binary_search(surface_patch_owner_scratch_.begin(),
                             surface_patch_owner_scratch_.end(),
                             record.logical_owner))
        surface_patch_dirty_scratch_.push_back(record.logical_owner);
    std::sort(surface_patch_dirty_scratch_.begin(),surface_patch_dirty_scratch_.end());
    surface_patch_dirty_scratch_.erase(
        std::unique(surface_patch_dirty_scratch_.begin(),
                    surface_patch_dirty_scratch_.end()),
        surface_patch_dirty_scratch_.end());
  }
  if(dual_topology){
    dual_patch_builder_.rebuild_index(mesh,sphere);
    if(!rebuild_all&&mesh.revision()!=surface_patch_mesh_revision_){
      surface_patch_incident_dirty_scratch_=surface_patch_dirty_scratch_;
      surface_patch_dirty_scratch_.clear();
      for(const auto owner:surface_patch_incident_dirty_scratch_)
        if(std::binary_search(surface_patch_owner_scratch_.begin(),
                              surface_patch_owner_scratch_.end(),owner))
          surface_patch_dirty_scratch_.push_back(owner);
      const auto add_dependent=[&](const auto& dependency){
        if(std::binary_search(surface_patch_incident_dirty_scratch_.begin(),
                              surface_patch_incident_dirty_scratch_.end(),
                              dependency.incident_owner))
          surface_patch_dirty_scratch_.push_back(dependency.patch_owner);
      };
      for(const auto dependency:dual_patch_dependencies_)add_dependent(dependency);
      for(const auto dependency:dual_patch_builder_.dependencies())
        add_dependent(dependency);
      std::sort(surface_patch_dirty_scratch_.begin(),
                surface_patch_dirty_scratch_.end());
      surface_patch_dirty_scratch_.erase(
          std::unique(surface_patch_dirty_scratch_.begin(),
                      surface_patch_dirty_scratch_.end()),
          surface_patch_dirty_scratch_.end());
    }
    dual_patch_dependencies_.assign(
        dual_patch_builder_.dependencies().begin(),
        dual_patch_builder_.dependencies().end());
    if(!surface_patch_dirty_scratch_.empty())
      dual_patch_builder_.generate_patches(
          mesh,sphere,surface_patch_dirty_scratch_,
          dual_patch_triangle_scratch_);
    else dual_patch_triangle_scratch_.clear();
  }else{
    dual_patch_dependencies_.clear();
    dual_patch_triangle_scratch_.clear();
  }
  if(mixed_depth_dual){
    mixed_depth_dual_patch_builder_.rebuild_index(mesh);
    if(!rebuild_all&&mesh.revision()!=surface_patch_mesh_revision_){
      surface_patch_incident_dirty_scratch_=surface_patch_dirty_scratch_;
      surface_patch_dirty_scratch_.clear();
      for(const auto owner:surface_patch_incident_dirty_scratch_)
        if(std::binary_search(surface_patch_owner_scratch_.begin(),
                              surface_patch_owner_scratch_.end(),owner))
          surface_patch_dirty_scratch_.push_back(owner);
      const auto add_dependent=[&](const auto& dependency){
        if(std::binary_search(surface_patch_incident_dirty_scratch_.begin(),
                              surface_patch_incident_dirty_scratch_.end(),
                              dependency.incident_owner))
          surface_patch_dirty_scratch_.push_back(dependency.patch_owner);
      };
      for(const auto dependency:mixed_depth_dual_patch_dependencies_)
        add_dependent(dependency);
      for(const auto dependency:mixed_depth_dual_patch_builder_.dependencies())
        add_dependent(dependency);
      std::sort(surface_patch_dirty_scratch_.begin(),
                surface_patch_dirty_scratch_.end());
      surface_patch_dirty_scratch_.erase(
          std::unique(surface_patch_dirty_scratch_.begin(),
                      surface_patch_dirty_scratch_.end()),
          surface_patch_dirty_scratch_.end());
    }
    mixed_depth_dual_patch_dependencies_.assign(
        mixed_depth_dual_patch_builder_.dependencies().begin(),
        mixed_depth_dual_patch_builder_.dependencies().end());
    if(!surface_patch_dirty_scratch_.empty())
      mixed_depth_dual_patch_builder_.generate_patches(
          mesh,sphere,surface_patch_dirty_scratch_,
          mixed_depth_dual_patch_triangle_scratch_);
    else mixed_depth_dual_patch_triangle_scratch_.clear();
  }else{
    mixed_depth_dual_patch_dependencies_.clear();
    mixed_depth_dual_patch_triangle_scratch_.clear();
  }
  surface_patch_metrics_.full_rebuild=rebuild_all;
  surface_patch_metrics_.dirty_owners=surface_patch_dirty_scratch_.size();
  if(four_hexahedra)four_hexahedra_patch_builder_.begin_update(field_revision);

  const auto normalize_free_ranges=[&]{
    std::sort(surface_patch_free_ranges_.begin(),surface_patch_free_ranges_.end(),
              [](const auto& left,const auto& right){return left.begin<right.begin;});
    std::size_t output{};
    for(const auto range:surface_patch_free_ranges_){
      if(range.count==0U)continue;
      if(output!=0U&&surface_patch_free_ranges_[output-1U].begin+
             surface_patch_free_ranges_[output-1U].count>=range.begin){
        auto& previous=surface_patch_free_ranges_[output-1U];
        previous.count=std::max(previous.begin+previous.count,
                                range.begin+range.count)-previous.begin;
      }else surface_patch_free_ranges_[output++]=range;
    }
    surface_patch_free_ranges_.resize(output);
    while(!surface_patch_free_ranges_.empty()){
      const auto range=surface_patch_free_ranges_.back();
      if(range.begin+range.count!=surface_patch_arena_.size())break;
      surface_patch_arena_.resize(range.begin);
      surface_patch_free_ranges_.pop_back();
    }
  };
  const auto retire=[&](const SurfacePatchRecord& record){
    if(record.triangle_capacity!=0U)
      surface_patch_free_ranges_.push_back(
          {record.triangle_begin,record.triangle_capacity});
    ++surface_patch_metrics_.retired_patches;
  };
  for(const auto& record:surface_patch_records_)
    if(!std::binary_search(surface_patch_owner_scratch_.begin(),
                           surface_patch_owner_scratch_.end(),
                           record.logical_owner))retire(record);
  normalize_free_ranges();

  const auto allocate=[&](std::size_t count){
    if(count==0U)return std::size_t{};
    std::size_t best=surface_patch_free_ranges_.size();
    for(std::size_t index=0;index<surface_patch_free_ranges_.size();++index){
      if(surface_patch_free_ranges_[index].count<count)continue;
      if(best==surface_patch_free_ranges_.size()||
         surface_patch_free_ranges_[index].count<surface_patch_free_ranges_[best].count)
        best=index;
    }
    if(best!=surface_patch_free_ranges_.size()){
      const auto begin=surface_patch_free_ranges_[best].begin;
      surface_patch_free_ranges_[best].begin+=count;
      surface_patch_free_ranges_[best].count-=count;
      if(surface_patch_free_ranges_[best].count==0U)
        surface_patch_free_ranges_.erase(
            surface_patch_free_ranges_.begin()+static_cast<std::ptrdiff_t>(best));
      return begin;
    }
    const auto begin=surface_patch_arena_.size();
    surface_patch_arena_.resize(begin+count);
    return begin;
  };
  const auto cells_for=[&](std::size_t owner_index){
    surface_patch_cell_scratch_.clear();
    const auto owner=surface_patch_owner_scratch_[owner_index];
    if(bcc){
      const auto offsets=mesh.logical_derived_offsets();
      const auto addresses=mesh.logical_derived_addresses();
      if(offsets.size()==surface_patch_owner_scratch_.size()+1U){
        const auto begin=offsets[owner_index],end=offsets[owner_index+1U];
        if(begin!=end){
          surface_patch_cell_scratch_.assign(
              addresses.begin()+static_cast<std::ptrdiff_t>(begin),
              addresses.begin()+static_cast<std::ptrdiff_t>(end));
          std::sort(surface_patch_cell_scratch_.begin(),
                    surface_patch_cell_scratch_.end());
          return;
        }
      }
      surface_patch_cell_scratch_.push_back(owner);
      return;
    }
    const auto begin=std::lower_bound(
        surface_patch_owner_cells_.begin(),surface_patch_owner_cells_.end(),owner,
        [](const auto& entry,tetra::TetId value){return entry.owner<value;});
    for(auto found=begin;found!=surface_patch_owner_cells_.end()&&
        found->owner==owner;++found)surface_patch_cell_scratch_.push_back(found->cell);
  };
  const auto patch_bounds=[&](SurfacePatchRecord& record){
    const double infinity=std::numeric_limits<double>::infinity();
    record.bounds_minimum={infinity,infinity,infinity};
    record.bounds_maximum={-infinity,-infinity,-infinity};
    const auto include=[&](tetra::Vec3 point){
      record.bounds_minimum.x=std::min(record.bounds_minimum.x,point.x);
      record.bounds_minimum.y=std::min(record.bounds_minimum.y,point.y);
      record.bounds_minimum.z=std::min(record.bounds_minimum.z,point.z);
      record.bounds_maximum.x=std::max(record.bounds_maximum.x,point.x);
      record.bounds_maximum.y=std::max(record.bounds_maximum.y,point.y);
      record.bounds_maximum.z=std::max(record.bounds_maximum.z,point.z);
    };
    for(std::size_t triangle=0;triangle<record.triangle_count;++triangle){
      const auto& value=surface_patch_arena_[record.triangle_begin+triangle];
      include(value.a);include(value.b);include(value.c);
    }
    if(record.triangle_count==0U)
      for(const auto vertex:mesh.tetrahedron(record.logical_owner).vertices)
        include(mesh.vertices()[vertex]);
  };

  surface_patch_record_scratch_.clear();
  surface_patch_record_scratch_.reserve(surface_patch_owner_scratch_.size());
  for(std::size_t owner_index=0;owner_index<surface_patch_owner_scratch_.size();++owner_index){
    const auto owner=surface_patch_owner_scratch_[owner_index];
    const auto found=std::lower_bound(
        surface_patch_records_.begin(),surface_patch_records_.end(),owner,
        [](const auto& record,tetra::TetId value){return record.logical_owner<value;});
    const bool retained=found!=surface_patch_records_.end()&&found->logical_owner==owner;
    const bool dirty=rebuild_all||!retained||std::binary_search(
        surface_patch_dirty_scratch_.begin(),surface_patch_dirty_scratch_.end(),owner);
    if(!dirty){
      if(four_hexahedra)four_hexahedra_patch_builder_.retain_owner(owner);
      surface_patch_record_scratch_.push_back(*found);
      ++surface_patch_metrics_.reused_patches;
      surface_patch_metrics_.reused_triangles+=found->triangle_count;
      continue;
    }
    surface_patch_triangle_scratch_.clear();
    if(dual_topology){
      const auto begin=std::lower_bound(
          dual_patch_triangle_scratch_.begin(),
          dual_patch_triangle_scratch_.end(),owner,
          [](const auto& value,tetra::TetId target){
            return value.patch_owner<target;
          });
      for(auto found=begin;found!=dual_patch_triangle_scratch_.end()&&
          found->patch_owner==owner;++found)
        surface_patch_triangle_scratch_.push_back(found->triangle);
    }else if(mixed_depth_dual){
      const auto begin=std::lower_bound(
          mixed_depth_dual_patch_triangle_scratch_.begin(),
          mixed_depth_dual_patch_triangle_scratch_.end(),owner,
          [](const auto& value,tetra::TetId target){
            return value.patch_owner<target;
          });
      for(auto found=begin;
          found!=mixed_depth_dual_patch_triangle_scratch_.end()&&
          found->patch_owner==owner;++found)
        surface_patch_triangle_scratch_.push_back(found->triangle);
    }else if(four_hexahedra){
      cells_for(owner_index);
      four_hexahedra_patch_builder_.extract_owner(
          mesh,sphere,owner,surface_patch_cell_scratch_,
          surface_patch_triangle_scratch_);
    }else{
      cells_for(owner_index);
      tetra::extract_isosurface(
          mesh,sphere,surface_patch_cell_scratch_,surface_patch_triangle_scratch_);
    }
    SurfacePatchRecord record;
    if(retained)record=*found;
    else record.logical_owner=owner;
    if(!retained||surface_patch_triangle_scratch_.size()>record.triangle_capacity){
      if(retained){retire(record);normalize_free_ranges();}
      record.triangle_begin=allocate(surface_patch_triangle_scratch_.size());
      record.triangle_capacity=surface_patch_triangle_scratch_.size();
    }
    record.mesh_revision=mesh.revision();
    record.field_revision=field_revision;
    record.topology_hash=surface_patch_topology_hash_scratch_[owner_index];
    record.triangle_count=surface_patch_triangle_scratch_.size();
    std::copy(surface_patch_triangle_scratch_.begin(),
              surface_patch_triangle_scratch_.end(),
              surface_patch_arena_.begin()+static_cast<std::ptrdiff_t>(record.triangle_begin));
    patch_bounds(record);
    surface_patch_record_scratch_.push_back(record);
    ++surface_patch_metrics_.rebuilt_patches;
    surface_patch_metrics_.generated_triangles+=surface_patch_triangle_scratch_.size();
  }
  surface_patch_records_.swap(surface_patch_record_scratch_);
  if(four_hexahedra){
    four_hexahedra_patch_builder_.finish_update();
    const auto& field_metrics=four_hexahedra_patch_builder_.metrics();
    surface_patch_metrics_.evaluated_field_samples=field_metrics.evaluated_samples;
    surface_patch_metrics_.reused_field_samples=field_metrics.reused_samples;
    surface_patch_metrics_.field_sample_records=field_metrics.records;
  }

  surface_patch_output_.clear();
  std::size_t triangle_count{};
  for(const auto& record:surface_patch_records_)triangle_count+=record.triangle_count;
  surface_patch_output_.reserve(triangle_count);
  for(const auto& record:surface_patch_records_)
    surface_patch_output_.insert(
        surface_patch_output_.end(),
        surface_patch_arena_.begin()+static_cast<std::ptrdiff_t>(record.triangle_begin),
        surface_patch_arena_.begin()+static_cast<std::ptrdiff_t>(
            record.triangle_begin+record.triangle_count));
  surface_patch_mesh_revision_=mesh.revision();
  surface_patch_field_revision_=field_revision;
  surface_patch_subdivision_method_=mesh.subdivision_method();
  surface_patch_dual_topology_=dual_topology;
  surface_patch_four_hexahedra_=four_hexahedra;
  surface_patch_mixed_depth_dual_=mixed_depth_dual;
  surface_patch_initialized_=true;
  surface_patch_metrics_.output_triangles=surface_patch_output_.size();
  surface_patch_metrics_.arena_slots=surface_patch_arena_.size();
  for(const auto range:surface_patch_free_ranges_)
    surface_patch_metrics_.free_slots+=range.count;
  surface_patch_metrics_.retained_bytes=
      surface_patch_records_.capacity()*sizeof(SurfacePatchRecord)+
      surface_patch_record_scratch_.capacity()*sizeof(SurfacePatchRecord)+
      surface_patch_arena_.capacity()*sizeof(tetra::Triangle)+
      surface_patch_output_.capacity()*sizeof(tetra::Triangle)+
      surface_patch_triangle_scratch_.capacity()*sizeof(tetra::Triangle)+
      surface_patch_free_ranges_.capacity()*sizeof(SurfacePatchFreeRange)+
      surface_patch_owner_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_topology_hash_scratch_.capacity()*sizeof(std::uint64_t)+
      surface_patch_dirty_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_incident_dirty_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_cell_scratch_.capacity()*sizeof(tetra::TetId)+
      surface_patch_owner_cells_.capacity()*sizeof(SurfacePatchOwnerCell)+
      dual_patch_builder_.retained_bytes()+
      four_hexahedra_patch_builder_.metrics().retained_bytes+
      mixed_depth_dual_patch_builder_.retained_bytes()+
      dual_patch_dependencies_.capacity()*
          sizeof(tetra::DualContourPatchDependency)+
      dual_patch_triangle_scratch_.capacity()*
          sizeof(tetra::DualContourPatchTriangle)+
      mixed_depth_dual_patch_dependencies_.capacity()*
          sizeof(tetra::MixedDepthDualPatchDependency)+
      mixed_depth_dual_patch_triangle_scratch_.capacity()*
          sizeof(tetra::MixedDepthDualPatchTriangle);
  surface_patch_metrics_.update_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-start).count();
}

bool SceneCache::update_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere, std::uint64_t sphere_revision,
                              SurfaceMethod surface_method, MaterialRule material_rule,
                              bool show_faces, bool show_hierarchy_edges,
                              bool show_surface_edges, bool depth_colours,
                              bool show_volume_edges, bool show_volume_faces,
                              double x_cut_position,
                              VolumeConnectionMethod volume_connection_method,
                              StencilConstruction stencil_construction,
                              StencilSelectionObjective stencil_selection_objective,
                              ScenePreparationOptions preparation,
                              std::span<const tetra::Triangle> surface_override,
                              std::uint64_t surface_override_revision) {
  const bool base_unchanged = has_subdivision_method_ && subdivision_method_ == mesh.subdivision_method() &&
      mesh_revision_ == mesh.revision() && sphere_revision_ == sphere_revision &&
      surface_override_revision_==surface_override_revision&&
      surface_method_ == surface_method && material_rule_ == material_rule &&
      volume_connection_method_ == volume_connection_method &&
      stencil_construction_ == stencil_construction &&
      stencil_selection_objective_ == stencil_selection_objective &&
      show_faces_ == show_faces && show_hierarchy_edges_ == show_hierarchy_edges &&
      show_surface_edges_ == show_surface_edges &&
      depth_colours_ == depth_colours &&
      (!preparation.surface_diagnostics||surface_diagnostics_available_) &&
      (!preparation.summary_statistics||summary_statistics_available_);
  const bool cut_unchanged = show_volume_faces_ == show_volume_faces &&
      show_volume_edges_ == show_volume_edges &&
      (!(show_volume_faces || show_volume_edges) || x_cut_position_ == x_cut_position);
  if (base_unchanged && cut_unchanged)
    return false;
  if (!base_unchanged) {
    const bool owner_patch_override=surface_override.empty()&&
        (surface_method==SurfaceMethod::marching_tetrahedra||
         surface_method==SurfaceMethod::lattice_cleaving||
         surface_method==SurfaceMethod::dual_contouring||
         surface_method==SurfaceMethod::four_hexahedra||
         surface_method==SurfaceMethod::mixed_depth_dual);
    if(owner_patch_override)
      update_surface_patches(mesh,sphere,sphere_revision,surface_method);
    else set_surface_patch_fallback(
        surface_override.empty(),surface_override.empty()&&
        !surface_patch_dependency(surface_method).patchable());
    const std::span<const tetra::Triangle> effective_surface=
        owner_patch_override?std::span<const tetra::Triangle>(surface_patch_output_):
                             surface_override;
    base_scene_ = prepare_scene(mesh, sphere, surface_method, material_rule, show_faces,
                               show_hierarchy_edges, show_surface_edges, depth_colours,
                               false, false, x_cut_position,volume_connection_method,
                               stencil_construction,stencil_selection_objective,preparation,
                               effective_surface,owner_patch_override);
    volume_classification_valid_ = false;
  }
  if(uses_connected_volume(volume_connection_method)&&
     (show_volume_faces||show_volume_edges)&&base_scene_.connected_volume_vertices.empty()){
    if(volume_connection_method==VolumeConnectionMethod::fixed_surface_shell)
      build_fixed_surface_shell(base_scene_,mesh,sphere,stencil_selection_objective);
    else build_adaptive_cleaved_volume(base_scene_,mesh,sphere,volume_connection_method,
                                       stencil_construction,stencil_selection_objective);
    if(surface_method==SurfaceMethod::surface_optimization&&
       volume_connection_method!=VolumeConnectionMethod::fixed_surface_shell)
      optimize_connected_volume_boundary(base_scene_,sphere);
  }
  scene_ = base_scene_;
  if (show_volume_faces || show_volume_edges) {
    if(uses_connected_volume(volume_connection_method)){
      const bool replace_surface=show_volume_faces;
      if(replace_surface){
        scene_.triangle_vertices.clear();
        scene_.visible_volume_face_triangles=0;
      }
      append_connected_volume(scene_,mesh,sphere,x_cut_position,show_volume_edges,show_volume_faces,
                              replace_surface,replace_surface&&show_surface_edges);
    }else{
      if (!volume_classification_valid_) {
        auto classification = classify_volume_cut_cells(mesh, sphere, material_rule, base_scene_.relations);
        volume_material_tetrahedra_ = std::move(classification.material_tetrahedra);
        volume_boundary_tetrahedra_ = std::move(classification.boundary_tetrahedra);
        volume_boundary_faces_ = std::move(classification.boundary_faces);
        volume_classification_valid_ = true;
      }
      append_selected_volume(scene_, mesh, volume_material_tetrahedra_,
                             volume_boundary_tetrahedra_,volume_boundary_faces_,
                             x_cut_position,show_volume_edges,show_volume_faces,
                             surface_method==SurfaceMethod::full_tetrahedra);
    }
  }
  mesh_revision_ = mesh.revision();
  subdivision_method_ = mesh.subdivision_method();
  has_subdivision_method_ = true;
  sphere_revision_ = sphere_revision;
  surface_override_revision_=surface_override_revision;
  show_faces_ = show_faces;
  show_hierarchy_edges_ = show_hierarchy_edges;
  show_surface_edges_ = show_surface_edges;
  show_volume_edges_ = show_volume_edges;
  show_volume_faces_ = show_volume_faces;
  x_cut_position_ = x_cut_position;
  depth_colours_ = depth_colours;
  material_rule_ = material_rule;
  surface_method_ = surface_method;
  volume_connection_method_ = volume_connection_method;
  stencil_construction_ = stencil_construction;
  stencil_selection_objective_ = stencil_selection_objective;
  surface_diagnostics_available_=base_scene_.surface_diagnostics_available;
  summary_statistics_available_=base_scene_.summary_statistics_available;
  ++scene_generation_;
  return true;
}

bool SceneCache::update_projection(const tetra::TetMesh& mesh, const tetra::Camera& camera, double pixel_threshold) {
  const bool same_camera = projected_camera_.position.x == camera.position.x &&
      projected_camera_.position.y == camera.position.y && projected_camera_.position.z == camera.position.z &&
      projected_camera_.forward.x == camera.forward.x &&
      projected_camera_.forward.y == camera.forward.y &&
      projected_camera_.forward.z == camera.forward.z &&
      projected_camera_.up.x == camera.up.x && projected_camera_.up.y == camera.up.y &&
      projected_camera_.up.z == camera.up.z &&
      projected_camera_.vertical_fov_radians == camera.vertical_fov_radians &&
      projected_camera_.viewport_height_pixels == camera.viewport_height_pixels &&
      projected_camera_.aspect_ratio == camera.aspect_ratio;
  if (projected_scene_generation_ == scene_generation_ && same_camera &&
      projected_pixel_threshold_ == pixel_threshold)
    return false;
  projection_ = prepare_projection_statistics(mesh, scene_, camera, pixel_threshold);
  projected_scene_generation_ = scene_generation_;
  projected_camera_ = camera;
  projected_pixel_threshold_ = pixel_threshold;
  ++projection_generation_;
  return true;
}

}  // namespace tetra_viewer
