#include "tetra_viewer/viewer_scene.hpp"

#include "tetra_core/green_templates.hpp"
#include "tetra_core/world_cut_directory.hpp"
#include "tetra_core/geometry_executor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tetra_viewer {

std::array<double,3> stone_pbr_colour(
    tetra::Vec3 normal,tetra::Vec3 view_direction) noexcept {
  const auto dot=[](tetra::Vec3 first,tetra::Vec3 second){
    return first.x*second.x+first.y*second.y+first.z*second.z;
  };
  const auto normalize=[&](tetra::Vec3 value){
    const double magnitude=std::sqrt(dot(value,value));
    return magnitude>1.0e-12?value/magnitude:tetra::Vec3{0.0,1.0,0.0};
  };
  normal=normalize(normal);view_direction=normalize(view_direction);
  const auto sun=world_sun_direction(default_world_sun_azimuth_radians,
                                     default_world_sun_elevation_radians);
  const auto half_direction=normalize(sun+view_direction);
  const double n_dot_l=std::max(0.0,dot(normal,sun));
  const double n_dot_v=std::max(0.0,dot(normal,view_direction));
  const double n_dot_h=std::max(0.0,dot(normal,half_direction));
  const double v_dot_h=std::max(0.0,dot(view_direction,half_direction));
  constexpr std::array<double,3> albedo{0.32,0.33,0.34};
  constexpr std::array<double,3> ground{0.08,0.075,0.07};
  constexpr std::array<double,3> sky{0.24,0.28,0.34};
  constexpr std::array<double,3> sun_radiance{2.8,2.60,2.30};
  constexpr double roughness=0.82;
  const double alpha=roughness*roughness;
  const double alpha_squared=alpha*alpha;
  const double denominator=n_dot_h*n_dot_h*(alpha_squared-1.0)+1.0;
  const double distribution=alpha_squared/
      std::max(std::acos(-1.0)*denominator*denominator,1.0e-5);
  const double k=(roughness+1.0)*(roughness+1.0)/8.0;
  const double geometry_l=n_dot_l/
      std::max(n_dot_l*(1.0-k)+k,1.0e-5);
  const double geometry_v=n_dot_v/
      std::max(n_dot_v*(1.0-k)+k,1.0e-5);
  const double sky_mix=std::clamp(normal.y*0.5+0.5,0.0,1.0);
  const double fresnel=0.04+0.96*std::pow(1.0-v_dot_h,5.0);
  std::array<double,3> result{};
  for(std::size_t channel=0;channel<3U;++channel){
    const double specular=distribution*geometry_l*geometry_v*fresnel/
        std::max(4.0*n_dot_l*n_dot_v,1.0e-5);
    const double diffuse=(1.0-fresnel)*albedo[channel]/std::acos(-1.0);
    const double environment=ground[channel]+
        (sky[channel]-ground[channel])*sky_mix;
    const double linear=albedo[channel]*environment+
        (diffuse+specular)*n_dot_l*sun_radiance[channel];
    result[channel]=std::pow(linear/(linear+1.0),1.0/2.2);
  }
  return result;
}

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

std::array<float,3> render_position(
    const PreparedScene& scene,tetra::Vec3 world_position) {
  const auto relative=world_position-scene.render_origin;
  return {static_cast<float>(relative.x),static_cast<float>(relative.y),
          static_cast<float>(relative.z)};
}

SceneVertex make_scene_vertex(
    const PreparedScene& scene,tetra::Vec3 world_position,
    std::array<float,3> colour,tetra::Vec3 normal={}) {
  SceneVertex vertex{};
  std::ranges::copy(render_position(scene,world_position),vertex.position);
  std::ranges::copy(colour,vertex.colour);
  vertex.normal[0]=static_cast<float>(normal.x);
  vertex.normal[1]=static_cast<float>(normal.y);
  vertex.normal[2]=static_cast<float>(normal.z);
  return vertex;
}

void prepare_surface_render_attributes(
    PreparedScene& scene,const tetra::Sphere* smooth_field=nullptr,
    tetra::GeometryExecutor* executor=nullptr) {
  const std::size_t triangle_count=scene.triangle_vertices.size()/3;
  if (triangle_count==0) return;
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto normalize=[&dot](tetra::Vec3 value){
    const double length=std::sqrt(dot(value,value));
    return length>1e-15?value/length:tetra::Vec3{};
  };
  const auto process=[&](std::size_t begin,std::size_t end,std::stop_token stop){
    for(std::size_t triangle=begin;triangle<end;++triangle){
      if(stop.stop_requested())return;
      const auto& first=scene.triangle_vertices[triangle*3];
      const auto& second=scene.triangle_vertices[triangle*3+1];
      const auto& third=scene.triangle_vertices[triangle*3+2];
      const tetra::Vec3 a{first.position[0],first.position[1],first.position[2]};
      const tetra::Vec3 b{second.position[0],second.position[1],second.position[2]};
      const tetra::Vec3 c{third.position[0],third.position[1],third.position[2]};
      auto geometric=face_normal(a,b,c);
      const tetra::Vec3 supplied{first.normal[0],first.normal[1],first.normal[2]};
      // Preserve the deliberately selected outward side, but derive the final
      // lighting vector from the submitted geometry. In particular, never
      // publish the raw area vector: its length shrinks quadratically under
      // refinement and was eventually mistaken for the zero-normal sentinel
      // by the fragment shader.
      if(dot(geometric,supplied)<0.0)geometric=geometric*-1.0;
      const auto normal=normalize(geometric);
      for(std::size_t vertex=0;vertex<3;++vertex){
        auto& output=scene.triangle_vertices[triangle*3+vertex];
        // Flat lighting and barycentric wireframes are render inputs, not
        // diagnostics, and must always be prepared.
        output.normal[0]=static_cast<float>(normal.x);
        output.normal[1]=static_cast<float>(normal.y);
        output.normal[2]=static_cast<float>(normal.z);
        auto smooth=normal;
        if(smooth_field!=nullptr){
          const tetra::Vec3 point{
              static_cast<double>(output.position[0])+scene.render_origin.x,
              static_cast<double>(output.position[1])+scene.render_origin.y,
              static_cast<double>(output.position[2])+scene.render_origin.z};
          smooth=normalize(smooth_field->normal(point));
          if(dot(smooth,normal)<0.0)smooth=smooth*-1.0;
        }
        output.smooth_normal[0]=static_cast<float>(smooth.x);
        output.smooth_normal[1]=static_cast<float>(smooth.y);
        output.smooth_normal[2]=static_cast<float>(smooth.z);
        output.barycentric[0]=vertex==0?1.0F:0.0F;
        output.barycentric[1]=vertex==1?1.0F:0.0F;
        output.barycentric[2]=vertex==2?1.0F:0.0F;
      }
    }
  };
  constexpr std::size_t parallel_attribute_threshold=1024U;
  if(executor&&executor->worker_count()>1U&&
     triangle_count>=parallel_attribute_threshold){
    const auto start=std::chrono::steady_clock::now();
    auto group=executor->make_group(
        0U,tetra::GeometryTaskPriority::interactive);
    const std::size_t block_count=std::max<std::size_t>(
        1U,executor->worker_count()*executor->configuration().blocks_per_worker);
    const std::size_t grain=std::max<std::size_t>(
        1U,(triangle_count+block_count-1U)/block_count);
    executor->parallel_for(group,0U,triangle_count,grain,process);
    executor->wait_and_help(group);
    scene.parallel_render_attribute_milliseconds+=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-start).count();
    scene.parallel_render_attribute_tasks+=(triangle_count+grain-1U)/grain;
  }else{
    process(0U,triangle_count,{});
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

template <typename KeyOf,typename AppendTriangle>
void triangulate_polygon(std::span<const std::size_t> polygon,KeyOf&& key_of,
                         AppendTriangle&& append_triangle) {
  if (polygon.size() == 3) {
    append_triangle(std::array<std::size_t, 3>{{polygon[0], polygon[1], polygon[2]}});
    return;
  }
  if (polygon.size() != 4) return;
  auto first_diagonal=std::array{key_of(polygon[0]),key_of(polygon[2])};
  auto second_diagonal=std::array{key_of(polygon[1]),key_of(polygon[3])};
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
  const auto global_vertices=tetra::make_world_vertex_identity_map(mesh);
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
  scene.connected_volume_global_keys.reserve(source_vertices.size());
  for(std::size_t vertex=0;vertex<source_vertices.size();++vertex)
    scene.connected_volume_global_keys.push_back(
        tetra::world_hierarchy_vertex_key(
            global_vertices.at(static_cast<tetra::VertexId>(vertex))));
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
    std::vector<tetra::WorldIncidentTetrahedron> incidents;
    incidents.reserve(mesh.conforming_volume().size());
    for(const auto id:mesh.conforming_volume().addresses()){
      const auto& tet=mesh.tetrahedron(id).vertices;
      tetra::WorldIncidentTetrahedron incident;
      for(std::size_t corner=0;corner<4U;++corner){
        incident.vertices[corner]=global_vertices.at(tet[corner]);
        incident.positions[corner]=source_vertices[tet[corner]];
      }
      incidents.push_back(incident);
    }
    const auto limits=tetra::world_safe_warp_limits(incidents);
    for(std::size_t vertex=0;vertex<source_vertices.size();++vertex){
      const auto key=global_vertices.at(static_cast<tetra::VertexId>(vertex));
      const auto found=std::ranges::lower_bound(
          limits,key,{},&tetra::WorldSafeWarpLimit::vertex);
      safe_warp_radius[vertex]=found!=limits.end()&&found->vertex==key?found->radius:0.0;
    }
  }

  struct GlobalCrossedEdge { tetra::WorldEdgeKey global{};PackedEdge local{}; };
  std::vector<GlobalCrossedEdge> crossed_keys;
  crossed_keys.reserve(mesh.conforming_volume().size()*2);
  for (const auto id : mesh.conforming_volume().addresses()) {
    const auto& tet = mesh.tetrahedron(id).vertices;
    std::array<bool, 4> inside{};
    for (std::size_t corner = 0; corner < 4; ++corner)
      inside[corner] = source_distances[tet[corner]] <= 0.0;
    for (const auto edge : edges) if (inside[edge[0]] != inside[edge[1]])
      crossed_keys.push_back({tetra::world_edge_key(global_vertices.at(tet[edge[0]]),
          global_vertices.at(tet[edge[1]])),pack_edge(tet[edge[0]],tet[edge[1]])});
  }
  std::ranges::sort(crossed_keys,{},&GlobalCrossedEdge::global);
  crossed_keys.erase(std::unique(crossed_keys.begin(),crossed_keys.end(),
      [](const auto& first,const auto& second){return first.global==second.global;}),
      crossed_keys.end());

  std::vector<CrossedEdge> crossings;
  crossings.reserve(crossed_keys.size());
  scene.connected_volume_vertices.reserve(
      source_vertices.size()+crossed_keys.size()+mesh.conforming_volume().size());
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto project_to_surface=[&](tetra::Vec3 point){return sphere.project_to_surface(point);};
  for (const auto& key : crossed_keys) {
    const auto endpoints = unpack_edge(key.local);
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
        vertex=first_distance<second_distance||(first_distance==second_distance&&
            global_vertices.at(endpoints[0])<global_vertices.at(endpoints[1]))
            ?endpoints[0]:endpoints[1];
      }else vertex=warp_first?endpoints[0]:endpoints[1];
      scene.connected_volume_vertices[vertex]=project_to_surface(source_vertices[vertex]);
    }else if (fraction <= snap_epsilon) vertex=endpoints[0];
    else if (fraction >= 1.0-snap_epsilon) vertex=endpoints[1];
    else {
      vertex=scene.connected_volume_vertices.size();
      scene.connected_volume_vertices.push_back(intersection);
      scene.connected_volume_vertex_kinds.push_back(ConnectedVertexKind::surface_intersection);
      scene.connected_volume_global_keys.push_back(tetra::world_edge_intersection_key(
          global_vertices.at(endpoints[0]),global_vertices.at(endpoints[1])));
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
    crossings.push_back({key.local,vertex});
  }
  std::ranges::sort(crossings,{},&CrossedEdge::key);
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
      const auto by_vertex=[&](std::size_t first,std::size_t second){
        return global_vertices.at(tet[first])<global_vertices.at(tet[second]);};
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
    std::ranges::sort(clipped_vertices,[&](std::size_t first,std::size_t second){
      return scene.connected_volume_global_keys[first]<scene.connected_volume_global_keys[second];
    });
    clipped_vertices.erase(std::unique(clipped_vertices.begin(),clipped_vertices.end()),clipped_vertices.end());
    tetra::Vec3 centre{};
    for(const auto vertex:clipped_vertices)centre=centre+scene.connected_volume_vertices[vertex];
    centre=centre/static_cast<double>(clipped_vertices.size());
    const std::size_t centre_id=scene.connected_volume_vertices.size();
    scene.connected_volume_vertices.push_back(centre);
    scene.connected_volume_vertex_kinds.push_back(ConnectedVertexKind::stencil_interior);
    std::array<tetra::WorldVertexKey,4> cell_keys{};
    for(std::size_t corner=0;corner<4U;++corner)
      cell_keys[corner]=global_vertices.at(tet[corner]);
    scene.connected_volume_global_keys.push_back(
        tetra::world_cell_interior_key(cell_keys));
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
      triangulate_polygon(polygon,[&](std::size_t vertex){
        return scene.connected_volume_global_keys[vertex];
      },[&](const auto& triangle){append_cell({centre_id,triangle[0],triangle[1],triangle[2]},id,true);});
      std::ranges::sort(face_crossings,[&](std::size_t first,std::size_t second){
        return scene.connected_volume_global_keys[first]<scene.connected_volume_global_keys[second];
      });
      face_crossings.erase(std::unique(face_crossings.begin(),face_crossings.end()),face_crossings.end());
      if(face_crossings.size()==2)interface_edges.push_back({face_crossings[0],face_crossings[1]});
    }

    std::vector<std::size_t> interface_polygon;
    if(!interface_edges.empty()){
      const auto less_vertex=[&](std::size_t first,std::size_t second){
        return scene.connected_volume_global_keys[first]<
               scene.connected_volume_global_keys[second];};
      const auto start=std::min_element(interface_edges.begin(),interface_edges.end(),
          [&](const auto& first,const auto& second){
            const auto first_min=less_vertex(first[1],first[0])?first[1]:first[0];
            const auto second_min=less_vertex(second[1],second[0])?second[1]:second[0];
            return less_vertex(first_min,second_min);
          });
      const std::size_t first=less_vertex((*start)[1],(*start)[0])?(*start)[1]:(*start)[0];
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
        std::sort(neighbours.begin(),neighbours.begin()+static_cast<std::ptrdiff_t>(neighbour_count),
                  less_vertex);
        std::size_t next=neighbours[0];
        if(next==previous&&neighbour_count>1)next=neighbours[1];
        previous=current;
        current=next;
        if(current==first)break;
      }
    }
    triangulate_polygon(interface_polygon,[&](std::size_t vertex){
      return scene.connected_volume_global_keys[vertex];
    },[&](const auto& triangle){append_cell({centre_id,triangle[0],triangle[1],triangle[2]},id,true);});
  }
}

// Improve the authoritative exterior of the connected volume in place. The
// surface and the boundary tetrahedra share these vertex indices, so every
// accepted move is immediately a conforming volume edit rather than a render
// mesh overlay. All adjacency is stored in compact offset/index arrays.
void optimize_connected_volume_boundary(PreparedScene& scene,
                                         const tetra::Sphere& sphere,
                                         std::span<const std::uint32_t> dependency_distance={},
                                         std::span<const std::size_t> evaluation_order={}) {
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  using Edge=std::array<std::size_t,2>;
  const std::size_t vertex_count=scene.connected_volume_vertices.size();
  const std::size_t tet_count=scene.connected_volume_tetrahedra.size();
  if(vertex_count==0||tet_count==0)return;
  scene.optimizer_passes=surface_optimizer_passes;
  scene.optimizer_dependency_halo_rings=surface_optimizer_dependency_halo_rings;

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
  const auto tet_global_key=[&](std::size_t tet_index){
    std::array<tetra::WorldDerivedVertexKey,4> key{};
    for(std::size_t corner=0;corner<4U;++corner)
      key[corner]=scene.connected_volume_global_keys[
          scene.connected_volume_tetrahedra[tet_index][corner]];
    std::ranges::sort(key);return key;
  };
  const auto face_global_key=[&](std::size_t face_index){
    std::array<tetra::WorldDerivedVertexKey,3> key{};
    for(std::size_t corner=0;corner<3U;++corner)
      key[corner]=scene.connected_volume_global_keys[boundary_faces[face_index][corner]];
    std::ranges::sort(key);return key;
  };
  for(std::size_t vertex=0;vertex<vertex_count;++vertex){
    std::sort(neighbors.begin()+static_cast<std::ptrdiff_t>(neighbor_offsets[vertex]),
              neighbors.begin()+static_cast<std::ptrdiff_t>(neighbor_offsets[vertex+1]),
              [&](std::size_t first,std::size_t second){
                return scene.connected_volume_global_keys[first]<
                       scene.connected_volume_global_keys[second];});
    std::sort(incident_tets.begin()+static_cast<std::ptrdiff_t>(incident_tet_offsets[vertex]),
              incident_tets.begin()+static_cast<std::ptrdiff_t>(incident_tet_offsets[vertex+1]),
              [&](std::size_t first,std::size_t second){
                return tet_global_key(first)<tet_global_key(second);});
    std::sort(incident_faces.begin()+static_cast<std::ptrdiff_t>(incident_face_offsets[vertex]),
              incident_faces.begin()+static_cast<std::ptrdiff_t>(incident_face_offsets[vertex+1]),
              [&](std::size_t first,std::size_t second){
                return face_global_key(first)<face_global_key(second);});
  }

  const auto tet_quality=[&](std::span<const tetra::Vec3> read_positions,
                             std::size_t tet_index,std::size_t moved_vertex,
                             tetra::Vec3 candidate){
    const auto& ids=scene.connected_volume_tetrahedra[tet_index];
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)
      points[corner]=ids[corner]==moved_vertex?candidate:read_positions[ids[corner]];
    return evaluate_tetrahedron_quality(points);
  };
  std::vector<double> initial_determinants(tet_count),initial_qualities(tet_count);
  scene.minimum_connected_tet_quality_before=1.0;
  scene.minimum_connected_tet_volume_surface_quality_before=1.0;
  scene.minimum_connected_tet_dihedral_sine_before=1.0;
  for(std::size_t tet_index=0;tet_index<tet_count;++tet_index){
    const auto quality=tet_quality(scene.connected_volume_vertices,tet_index,
                                   std::numeric_limits<std::size_t>::max(),{});
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
  const auto triangle_fairness=[&](std::span<const tetra::Vec3> read_positions,
                                   const std::array<std::size_t,3>& ids,
                                   std::size_t moved_vertex,tetra::Vec3 candidate){
    std::array<tetra::Vec3,3> points{};
    for(std::size_t corner=0;corner<3;++corner)
      points[corner]=ids[corner]==moved_vertex?candidate:read_positions[ids[corner]];
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
  const auto valid_move=[&](std::span<const tetra::Vec3> read_positions,
                            std::size_t vertex,tetra::Vec3 candidate){
    for(std::size_t offset=incident_tet_offsets[vertex];offset<incident_tet_offsets[vertex+1];++offset){
      const auto tet_index=incident_tets[offset];
      const auto quality=tet_quality(read_positions,tet_index,vertex,candidate);
      if(quality.signed_six_volume<=initial_determinants[tet_index]*1.0e-6||
         quality.mean_ratio<std::max(1.0e-5,initial_qualities[tet_index]*0.5))return false;
    }
    double fairness_before{},fairness_after{};
    for(std::size_t offset=incident_face_offsets[vertex];offset<incident_face_offsets[vertex+1];++offset){
      const auto& ids=boundary_faces[incident_faces[offset]];
      std::array<tetra::Vec3,3> points{};
      for(std::size_t corner=0;corner<3;++corner)
        points[corner]=ids[corner]==vertex?candidate:read_positions[ids[corner]];
      const auto normal=face_normal(points[0],points[1],points[2]);
      const double area_squared=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
      const auto centre=(points[0]+points[1]+points[2])/3.0;
      const auto outward=sphere.normal(centre);
      if(area_squared<1.0e-24||normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<=0.0)
        return false;
      fairness_before+=triangle_fairness(
          read_positions,ids,std::numeric_limits<std::size_t>::max(),{});
      fairness_after+=triangle_fairness(read_positions,ids,vertex,candidate);
    }
    return fairness_after<=fairness_before+1.0e-12;
  };
  const auto project_to_surface=[&](tetra::Vec3 point){return sphere.project_to_surface(point);};
  constexpr std::size_t line_search_steps=10;
  constexpr double relaxation=0.35;
  run_bounded_jacobi(scene.connected_volume_vertices,surface_optimizer_passes,
    dependency_distance,evaluation_order,
    [&](std::span<const tetra::Vec3> previous,std::size_t vertex,std::uint32_t)
        ->std::optional<tetra::Vec3>{
      const auto degree=neighbor_offsets[vertex+1]-neighbor_offsets[vertex];
      if(degree<3)return std::nullopt;
      tetra::Vec3 average{};
      for(std::size_t offset=neighbor_offsets[vertex];offset<neighbor_offsets[vertex+1];++offset)
        average=average+previous[neighbors[offset]];
      average=average/static_cast<double>(degree);
      const auto original=previous[vertex];
      const auto target=project_to_surface(original*(1.0-relaxation)+average*relaxation);
      double step=1.0;
      for(std::size_t attempt=0;attempt<line_search_steps;++attempt,step*=0.5){
        const auto candidate=project_to_surface(original*(1.0-step)+target*step);
        if(valid_move(previous,vertex,candidate)){
          ++scene.optimized_volume_boundary_vertices;
          return candidate;
        }
      }
      ++scene.rejected_volume_boundary_moves;
      return std::nullopt;
    });
  scene.minimum_connected_tet_quality_after=1.0;
  scene.minimum_connected_tet_volume_surface_quality_after=1.0;
  scene.minimum_connected_tet_dihedral_sine_after=1.0;
  scene.minimum_connected_tet_dihedral_degrees_after=180.0;
  scene.maximum_connected_tet_dihedral_degrees_after=0.0;
  for(std::size_t tet_index=0;tet_index<tet_count;++tet_index){
    const auto quality=tet_quality(scene.connected_volume_vertices,tet_index,
                                   std::numeric_limits<std::size_t>::max(),{});
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
  const auto append_vertex_key=[&](const tetra::WorldDerivedVertexKey& key){
    append_hash(static_cast<std::uint8_t>(key.kind));append_hash(key.basis_count);
    for(std::size_t index=0;index<key.basis_count;++index){
      append_hash(static_cast<std::uint64_t>(key.basis[index].x));
      append_hash(static_cast<std::uint64_t>(key.basis[index].y));
      append_hash(static_cast<std::uint64_t>(key.basis[index].z));
      append_hash(key.basis[index].denominator_exponent);
    }
  };
  std::vector<std::size_t> surface_order;
  for(std::size_t vertex=0;vertex<vertex_count;++vertex)
    if(scene.connected_volume_surface_vertices[vertex]!=0U)surface_order.push_back(vertex);
  std::ranges::sort(surface_order,[&](std::size_t first,std::size_t second){
    return scene.connected_volume_global_keys[first]<scene.connected_volume_global_keys[second];
  });
  for(const auto vertex:surface_order){
    append_vertex_key(scene.connected_volume_global_keys[vertex]);
    const auto point=scene.connected_volume_vertices[vertex];
    append_hash(std::bit_cast<std::uint64_t>(point.x));
    append_hash(std::bit_cast<std::uint64_t>(point.y));
    append_hash(std::bit_cast<std::uint64_t>(point.z));
  }
  std::vector<std::array<tetra::WorldDerivedVertexKey,3>> boundary_keys;
  boundary_keys.reserve(boundary_faces.size());
  for(const auto& face:boundary_faces){
    std::array<tetra::WorldDerivedVertexKey,3> key{{
        scene.connected_volume_global_keys[face[0]],
        scene.connected_volume_global_keys[face[1]],
        scene.connected_volume_global_keys[face[2]]}};
    std::ranges::sort(key);boundary_keys.push_back(key);
  }
  std::sort(boundary_keys.begin(),boundary_keys.end());
  for(const auto& face:boundary_keys)
    for(const auto& vertex:face)append_vertex_key(vertex);
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
        for (const auto point : {a,b,c}) scene.triangle_vertices.push_back(
            make_scene_vertex(scene,point,{0.24F,0.78F,0.48F},normal));
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
      scene.triangle_vertices.push_back(
          make_scene_vertex(scene,point,{0.22F,0.72F,0.52F},normal));
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
      scene.triangle_vertices.push_back(
          make_scene_vertex(scene,point,{0.32F,0.76F,0.42F},normal));
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
      scene.triangle_vertices.push_back(
          make_scene_vertex(scene,point,{0.26F,0.72F,0.56F},normal));
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
      scene.triangle_vertices.push_back(
          make_scene_vertex(scene,point,{0.20F,0.74F,0.60F},normal));
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
      scene.triangle_vertices.push_back(
          make_scene_vertex(scene,point,{0.80F,0.58F,0.24F},normal));
  }
}

struct OptimizedSurface {
  std::vector<tetra::Vec3> positions;
  std::vector<std::array<std::size_t,3>> triangles;
  std::vector<std::array<tetra::VertexId,2>> source_edges;
  std::vector<tetra::WorldDerivedVertexKey> global_keys;
};

void optimize_surface_graph(
    PreparedScene& scene,const tetra::Sphere& sphere,OptimizedSurface& surface,
    std::span<const std::uint32_t> dependency_distance={},
    std::span<const std::size_t> evaluation_order={},
    tetra::GeometryExecutor* executor=nullptr) {
  auto& positions=surface.positions;
  const auto& triangles=surface.triangles;
  const auto& global_keys=surface.global_keys;
  std::vector<std::array<std::size_t,2>> edges;
  edges.reserve(triangles.size()*3U);
  for(const auto& triangle:triangles)
    for(auto edge:std::array<std::array<std::size_t,2>,3>{{
        {{triangle[0],triangle[1]}},{{triangle[1],triangle[2]}},
        {{triangle[2],triangle[0]}}}}){
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      edges.push_back(edge);
    }
  std::ranges::sort(edges);
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  std::vector<std::size_t> neighbor_offsets(positions.size()+1U);
  std::vector<std::size_t> incident_offsets(positions.size()+1U);
  for(const auto& edge:edges){
    ++neighbor_offsets[edge[0]+1U];++neighbor_offsets[edge[1]+1U];
  }
  for(const auto& triangle:triangles)
    for(const auto vertex:triangle)++incident_offsets[vertex+1U];
  for(std::size_t index=1;index<neighbor_offsets.size();++index){
    neighbor_offsets[index]+=neighbor_offsets[index-1U];
    incident_offsets[index]+=incident_offsets[index-1U];
  }
  std::vector<std::size_t> neighbors(neighbor_offsets.back());
  std::vector<std::size_t> incidents(incident_offsets.back());
  auto neighbor_cursor=neighbor_offsets,incident_cursor=incident_offsets;
  for(const auto& edge:edges){
    neighbors[neighbor_cursor[edge[0]]++]=edge[1];
    neighbors[neighbor_cursor[edge[1]]++]=edge[0];
  }
  for(std::size_t triangle=0;triangle<triangles.size();++triangle)
    for(const auto vertex:triangles[triangle])
      incidents[incident_cursor[vertex]++]=triangle;
  const auto triangle_global_key=[&](std::size_t triangle){
    std::array<tetra::WorldDerivedVertexKey,3> key{{
        global_keys[triangles[triangle][0]],global_keys[triangles[triangle][1]],
        global_keys[triangles[triangle][2]]}};
    std::ranges::sort(key);return key;
  };
  for(std::size_t vertex=0;vertex<positions.size();++vertex){
    std::sort(neighbors.begin()+static_cast<std::ptrdiff_t>(neighbor_offsets[vertex]),
              neighbors.begin()+static_cast<std::ptrdiff_t>(neighbor_offsets[vertex+1U]),
              [&](std::size_t first,std::size_t second){
                return global_keys[first]<global_keys[second];
              });
    std::sort(incidents.begin()+static_cast<std::ptrdiff_t>(incident_offsets[vertex]),
              incidents.begin()+static_cast<std::ptrdiff_t>(incident_offsets[vertex+1U]),
              [&](std::size_t first,std::size_t second){
                return triangle_global_key(first)<triangle_global_key(second);
              });
  }
  const auto triangle_fairness=[](const std::array<tetra::Vec3,3>& points){
    double energy{};
    constexpr double target=1.0471975511965977462;
    for(std::size_t corner=0;corner<3U;++corner){
      const auto a=points[(corner+1U)%3U]-points[corner];
      const auto b=points[(corner+2U)%3U]-points[corner];
      const double aa=a.x*a.x+a.y*a.y+a.z*a.z;
      const double bb=b.x*b.x+b.y*b.y+b.z*b.z;
      if(aa<=1.0e-30||bb<=1.0e-30)
        return std::numeric_limits<double>::infinity();
      const double cosine=std::clamp(
          (a.x*b.x+a.y*b.y+a.z*b.z)/std::sqrt(aa*bb),-1.0,1.0);
      const double difference=std::acos(cosine)-target;
      energy+=difference*difference;
    }
    return energy;
  };
  const auto incident_fairness=[&](std::span<const tetra::Vec3> read_positions,
                                   std::size_t vertex){
    double fairness{};
    for(std::size_t offset=incident_offsets[vertex];
        offset<incident_offsets[vertex+1U];++offset){
      const auto& ids=triangles[incidents[offset]];
      fairness+=triangle_fairness({{read_positions[ids[0]],
          read_positions[ids[1]],read_positions[ids[2]]}});
    }
    return fairness;
  };
  const auto valid_move=[&](std::span<const tetra::Vec3> read_positions,
                            std::size_t vertex,tetra::Vec3 candidate,
                            double fairness_before){
    double fairness_after{};
    for(std::size_t offset=incident_offsets[vertex];
        offset<incident_offsets[vertex+1U];++offset){
      const auto& ids=triangles[incidents[offset]];
      std::array<tetra::Vec3,3> points{{read_positions[ids[0]],
          read_positions[ids[1]],read_positions[ids[2]]}};
      for(std::size_t corner=0;corner<3U;++corner)
        if(ids[corner]==vertex)points[corner]=candidate;
      fairness_after+=triangle_fairness(points);
      const auto normal=face_normal(points[0],points[1],points[2]);
      const double area2=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
      const auto centre=(points[0]+points[1]+points[2])/3.0;
      const auto outward=sphere.normal(centre);
      if(area2<1.0e-20||
         normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<=1.0e-14)
        return false;
    }
    return fairness_after<=fairness_before+1.0e-12;
  };
  scene.optimizer_passes=surface_optimizer_passes;
  scene.optimizer_dependency_halo_rings=surface_optimizer_dependency_halo_rings;
  constexpr std::size_t line_search_steps=10U;
  constexpr double relaxation=0.35;
  const auto propose=[&](std::span<const tetra::Vec3> previous,
                         std::size_t vertex,std::size_t& rejected)
      ->std::optional<tetra::Vec3>{
        tetra::Vec3 average{};
        const auto degree=neighbor_offsets[vertex+1U]-neighbor_offsets[vertex];
        if(degree<3U)return std::nullopt;
        for(std::size_t offset=neighbor_offsets[vertex];
            offset<neighbor_offsets[vertex+1U];++offset)
          average=average+previous[neighbors[offset]];
        average=average/static_cast<double>(degree);
        const auto original=previous[vertex];
        const double fairness_before=incident_fairness(previous,vertex);
        auto target=sphere.project_to_surface(
            original*(1.0-relaxation)+average*relaxation);
        double step=1.0;
        for(std::size_t attempt=0;attempt<line_search_steps;++attempt,step*=0.5){
          auto candidate=sphere.project_to_surface(
              original*(1.0-step)+target*step);
          if(valid_move(previous,vertex,candidate,fairness_before))return candidate;
        }
        ++rejected;
        return std::nullopt;
      };
  const std::size_t work_items=evaluation_order.empty()?
      positions.size():evaluation_order.size();
  // A retained camera patch is commonly only a few thousand vertices. The
  // previous 4096-item grain left the expensive projected line searches on
  // one or two cores even though the Jacobi pass is read-only per vertex.
  const std::size_t worker_count=std::max<std::size_t>(1U,std::min<std::size_t>(
      10U,std::min<std::size_t>(std::thread::hardware_concurrency(),
                                (work_items+511U)/512U)));
  for(std::uint32_t pass=0;pass<surface_optimizer_passes;++pass){
    const auto previous=positions;auto next=previous;
    std::vector<std::size_t> rejected(worker_count);
    const auto work=[&](std::size_t worker,std::size_t begin,std::size_t end){
      for(std::size_t item=begin;item<end;++item){
        const auto vertex=evaluation_order.empty()?item:evaluation_order[item];
        if(!dependency_distance.empty()&&
           dependency_distance[vertex]>=surface_optimizer_passes-pass)continue;
        if(const auto candidate=propose(previous,vertex,rejected[worker]))
          next[vertex]=*candidate;
      }
    };
    if(worker_count==1U)work(0U,0U,work_items);
    else if(executor!=nullptr){
      auto group=executor->make_group(
          pass,tetra::GeometryTaskPriority::publication_critical);
      for(std::size_t worker=0;worker<worker_count;++worker){
        const auto begin=work_items*worker/worker_count;
        const auto end=work_items*(worker+1U)/worker_count;
        executor->submit(group,[&,worker,begin,end](std::stop_token stop){
          if(!stop.stop_requested())work(worker,begin,end);
        });
      }
      executor->wait_and_help(group);
    }
    else{
      std::vector<std::thread> workers;workers.reserve(worker_count);
      for(std::size_t worker=0;worker<worker_count;++worker){
        const auto begin=work_items*worker/worker_count;
        const auto end=work_items*(worker+1U)/worker_count;
        workers.emplace_back(work,worker,begin,end);
      }
      for(auto& worker:workers)worker.join();
    }
    scene.rejected_surface_moves+=
        std::accumulate(rejected.begin(),rejected.end(),std::size_t{});
    positions=std::move(next);
  }
  scene.optimized_surface_vertices=positions.size();
}

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
                                         const std::vector<ConnectedFace>* supplied_boundary=nullptr,
                                         bool optimize=true) {
  std::vector<tetra::Triangle> input;
  std::vector<std::array<std::array<tetra::VertexId,2>,3>> input_source_edges;
  std::vector<std::array<tetra::WorldDerivedVertexKey,3>> input_global_keys;
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
        std::array<tetra::WorldDerivedVertexKey,3> global_keys{{
            topology_source->connected_volume_global_keys[ids[0]],
            topology_source->connected_volume_global_keys[ids[1]],
            topology_source->connected_volume_global_keys[ids[2]]}};
        const auto normal=face_normal(a,b,c),centre=(a+b+c)/3.0;
        const auto outward=sphere.normal(centre);
        if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0){
          std::swap(b,c);std::swap(source_edges[1],source_edges[2]);
          std::swap(global_keys[1],global_keys[2]);
        }
        input.push_back({a,b,c});
        input_source_edges.push_back(source_edges);
        input_global_keys.push_back(global_keys);
    }
  }else input=tetra::extract_isosurface(mesh,sphere);
  using PointKey=std::array<long long,3>;
  struct Corner {
    PointKey key{};
    std::array<tetra::VertexId,2> source_edge{};
    tetra::WorldDerivedVertexKey global_key{};
    tetra::Vec3 point{};
    std::size_t triangle{},corner{};
  };
  std::vector<Corner> corners;
  corners.reserve(input.size()*3);
  const auto point_key=[](tetra::Vec3 point){
    constexpr double scale=1.0e10;
    return PointKey{{std::llround(point.x*scale),std::llround(point.y*scale),std::llround(point.z*scale)}};
  };
  const auto global_vertices=tetra::make_world_vertex_identity_map(mesh);
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
      tetra::WorldDerivedVertexKey global_key{};
      if(input_source_edges.size()==input.size())edge=input_source_edges[triangle][corner];
      else{
        const auto source=std::lower_bound(provenance.begin(),provenance.end(),key,
            [](const CrossingProvenance& item,const PointKey& value){return item.key<value;});
        edge=source!=provenance.end()&&source->key==key?source->edge:
            std::array<tetra::VertexId,2>{{std::numeric_limits<tetra::VertexId>::max(),
                                          std::numeric_limits<tetra::VertexId>::max()}};
      }
      if(input_global_keys.size()==input.size())global_key=input_global_keys[triangle][corner];
      else if(edge[0]!=std::numeric_limits<tetra::VertexId>::max())
        global_key=tetra::world_edge_intersection_key(
            global_vertices.at(edge[0]),global_vertices.at(edge[1]));
      else throw std::logic_error("optimized surface vertex lacks global provenance");
      corners.push_back({key,edge,global_key,points[corner],triangle,corner});
    }
  }
  std::sort(corners.begin(),corners.end(),[](const Corner& a,const Corner& b){
    return a.global_key<b.global_key||(a.global_key==b.global_key&&a.key<b.key);
  });
  std::vector<tetra::Vec3> positions;
  std::vector<std::array<tetra::VertexId,2>> source_edges;
  std::vector<tetra::WorldDerivedVertexKey> global_keys;
  std::vector<std::array<std::size_t,3>> triangles(input.size());
  for(std::size_t begin=0;begin<corners.size();){
    std::size_t end=begin+1;
    while(end<corners.size()&&corners[end].global_key==corners[begin].global_key&&
          corners[end].key==corners[begin].key)++end;
    const std::size_t vertex=positions.size();
    positions.push_back(corners[begin].point);
    source_edges.push_back(corners[begin].source_edge);
    global_keys.push_back(corners[begin].global_key);
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
  OptimizedSurface surface{std::move(positions),std::move(triangles),
                           std::move(source_edges),std::move(global_keys)};
  if(optimize)optimize_surface_graph(scene,sphere,surface);
  return surface;
}

std::uint64_t hash_indexed_surface(const OptimizedSurface& surface){
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  std::uint64_t hash=offset;
  const auto append=[&](std::uint64_t value){hash^=value;hash*=prime;};
  const auto append_key=[&](const tetra::WorldDerivedVertexKey& key){
    append(static_cast<std::uint8_t>(key.kind));append(key.basis_count);
    for(std::size_t index=0;index<key.basis_count;++index){
      append(static_cast<std::uint64_t>(key.basis[index].x));
      append(static_cast<std::uint64_t>(key.basis[index].y));
      append(static_cast<std::uint64_t>(key.basis[index].z));
      append(key.basis[index].denominator_exponent);
    }
  };
  std::vector<std::size_t> order(surface.positions.size());
  std::iota(order.begin(),order.end(),0U);
  std::ranges::sort(order,[&](std::size_t first,std::size_t second){
    return surface.global_keys[first]<surface.global_keys[second];
  });
  for(const auto vertex:order){
    append_key(surface.global_keys[vertex]);
    const auto point=surface.positions[vertex];
    append(std::bit_cast<std::uint64_t>(point.x));
    append(std::bit_cast<std::uint64_t>(point.y));
    append(std::bit_cast<std::uint64_t>(point.z));
  }
  std::vector<std::array<tetra::WorldDerivedVertexKey,3>> triangles;
  triangles.reserve(surface.triangles.size());
  for(const auto triangle:surface.triangles){
    std::array<tetra::WorldDerivedVertexKey,3> key{{surface.global_keys[triangle[0]],
        surface.global_keys[triangle[1]],surface.global_keys[triangle[2]]}};
    std::ranges::sort(key);triangles.push_back(key);
  }
  std::ranges::sort(triangles);
  for(const auto& triangle:triangles)for(const auto& vertex:triangle)append_key(vertex);
  return hash;
}

BlockedDerivedSurfaceBuild assemble_blocked_snapshots(
    std::vector<tetra::WorldDerivedSurfaceSnapshot> snapshots) {
  BlockedDerivedSurfaceBuild result;
  result.snapshots=std::move(snapshots);
  std::ranges::sort(result.snapshots,{},&tetra::WorldDerivedSurfaceSnapshot::id);
  if(std::ranges::adjacent_find(result.snapshots,[](const auto& first,const auto& second){
       return first.id==second.id;
     })!=result.snapshots.end())
    throw std::invalid_argument("blocked surface assembly has duplicate snapshots");
  for(const auto& snapshot:result.snapshots){
    result.vertices.insert(result.vertices.end(),snapshot.vertices.begin(),
                           snapshot.vertices.end());
    result.triangles.insert(result.triangles.end(),snapshot.triangles.begin(),
                            snapshot.triangles.end());
  }
  std::ranges::sort(result.vertices,{},&tetra::WorldSurfaceVertex::key);
  std::vector<tetra::WorldSurfaceVertex> unique_vertices;
  unique_vertices.reserve(result.vertices.size());
  for(const auto& vertex:result.vertices){
    if(unique_vertices.empty()||unique_vertices.back().key!=vertex.key){
      unique_vertices.push_back(vertex);continue;
    }
    const auto& existing=unique_vertices.back().position;
    if(std::bit_cast<std::uint64_t>(existing.x)!=
           std::bit_cast<std::uint64_t>(vertex.position.x)||
       std::bit_cast<std::uint64_t>(existing.y)!=
           std::bit_cast<std::uint64_t>(vertex.position.y)||
       std::bit_cast<std::uint64_t>(existing.z)!=
           std::bit_cast<std::uint64_t>(vertex.position.z))
      throw std::logic_error("blocked surface patches disagree on a shared vertex");
  }
  result.vertices=std::move(unique_vertices);
  std::ranges::sort(result.triangles);
  if(std::ranges::adjacent_find(result.triangles)!=result.triangles.end())
    throw std::logic_error("blocked surface publishes a duplicate triangle");

  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  std::uint64_t hash=offset;
  const auto append=[&](std::uint64_t value){hash^=value;hash*=prime;};
  const auto append_key=[&](const tetra::WorldDerivedVertexKey& key){
    append(static_cast<std::uint8_t>(key.kind));append(key.basis_count);
    for(std::size_t index=0;index<key.basis_count;++index){
      append(static_cast<std::uint64_t>(key.basis[index].x));
      append(static_cast<std::uint64_t>(key.basis[index].y));
      append(static_cast<std::uint64_t>(key.basis[index].z));
      append(key.basis[index].denominator_exponent);
    }
  };
  for(const auto& vertex:result.vertices){
    append_key(vertex.key);
    append(std::bit_cast<std::uint64_t>(vertex.position.x));
    append(std::bit_cast<std::uint64_t>(vertex.position.y));
    append(std::bit_cast<std::uint64_t>(vertex.position.z));
  }
  std::vector<std::array<tetra::WorldDerivedVertexKey,3>> triangle_keys;
  triangle_keys.reserve(result.triangles.size());
  for(const auto& triangle:result.triangles){
    auto keys=triangle.vertices;std::ranges::sort(keys);
    triangle_keys.push_back(keys);
  }
  std::ranges::sort(triangle_keys);
  for(const auto& triangle:triangle_keys)
    for(const auto& key:triangle)append_key(key);
  result.canonical_surface_hash=hash;
  return result;
}

template<class ChangedBlocks,class CurrentIndex>
BlockedDerivedSurfaceBuild assemble_blocked_snapshots_incremental(
    std::vector<tetra::WorldDerivedSurfaceSnapshot> snapshots,
    const SparseWorldSurfaceCache& cache,
    const ChangedBlocks& changed,
    std::vector<SparseWorldSurfaceCache::CountedSurfaceVertex>& next_vertices,
    std::vector<SparseWorldSurfaceCache::CountedSurfaceTriangle>& next_triangles,
    bool assemble_flat_output,
    std::span<const tetra::WorldDerivedVertexKey> stable_keys={},
    std::span<const std::uint32_t> current_to_stable={},
    const CurrentIndex* current_index=nullptr) {
  std::ranges::sort(snapshots,{},&tetra::WorldDerivedSurfaceSnapshot::id);
  struct VertexDelta {
    tetra::WorldSurfaceVertex vertex;
    std::int32_t removed{},added{};
  };
  struct TriangleDelta {
    tetra::WorldSurfaceTriangle triangle;
    std::array<tetra::WorldDerivedVertexKey,3> canonical_vertices;
    std::int32_t removed{},added{};
  };
  std::vector<VertexDelta> vertex_deltas;
  std::vector<TriangleDelta> triangle_deltas;
  const bool bootstrap=cache.assembled_vertices.empty()&&
      cache.assembled_triangles.empty();
  const bool stable_mode=!stable_keys.empty();
  if(!bootstrap&&stable_mode!=
      cache.assembled_triangles_use_optimizer_stable_ids)
    throw std::logic_error("incremental surface triangle identity mode changed");
  const auto append_snapshot=[&](const auto& snapshot,bool addition){
    for(const auto& vertex:snapshot.vertices)
      vertex_deltas.push_back({vertex,addition?0:1,addition?1:0});
    for(const auto& triangle:snapshot.triangles)
      triangle_deltas.push_back({triangle,triangle.canonical_vertices(),
          addition?0:1,addition?1:0});
  };
  if(!bootstrap)for(const auto& snapshot:cache.snapshots){
    const auto current=std::ranges::lower_bound(
        snapshots,snapshot.id,{},&tetra::WorldDerivedSurfaceSnapshot::id);
    if(changed.contains(snapshot.id)||current==snapshots.end()||
       current->id!=snapshot.id)append_snapshot(snapshot,false);
  }
  for(const auto& snapshot:snapshots){
    const auto previous=std::ranges::lower_bound(
        cache.snapshots,snapshot.id,{},&tetra::WorldDerivedSurfaceSnapshot::id);
    if(bootstrap||changed.contains(snapshot.id)||previous==cache.snapshots.end()||
       previous->id!=snapshot.id)append_snapshot(snapshot,true);
  }
  std::ranges::sort(vertex_deltas,{},
      [](const auto& delta){return delta.vertex.key;});
  std::vector<VertexDelta> grouped_vertices;
  for(const auto& delta:vertex_deltas){
    if(grouped_vertices.empty()||
       grouped_vertices.back().vertex.key!=delta.vertex.key){
      grouped_vertices.push_back(delta);continue;
    }
    auto& grouped=grouped_vertices.back();
    if(delta.added>0){
      const auto& first=grouped.vertex.position;
      const auto& second=delta.vertex.position;
      if(grouped.added>0&&(
          std::bit_cast<std::uint64_t>(first.x)!=std::bit_cast<std::uint64_t>(second.x)||
          std::bit_cast<std::uint64_t>(first.y)!=std::bit_cast<std::uint64_t>(second.y)||
          std::bit_cast<std::uint64_t>(first.z)!=std::bit_cast<std::uint64_t>(second.z)))
        throw std::logic_error("blocked surface patches disagree on a shared vertex");
      grouped.vertex=delta.vertex;
    }
    grouped.removed+=delta.removed;grouped.added+=delta.added;
  }
  next_vertices.clear();
  next_vertices.reserve(cache.assembled_vertices.size()+grouped_vertices.size());
  auto old_vertex=cache.assembled_vertices.begin();
  for(const auto& delta:grouped_vertices){
    while(old_vertex!=cache.assembled_vertices.end()&&
          old_vertex->vertex.key<delta.vertex.key)
      next_vertices.push_back(*old_vertex++);
    const bool exists=old_vertex!=cache.assembled_vertices.end()&&
        old_vertex->vertex.key==delta.vertex.key;
    const auto old_references=exists?old_vertex->references:0U;
    if(delta.removed<0||delta.added<0||
       static_cast<std::uint32_t>(delta.removed)>old_references)
      throw std::logic_error("incremental surface vertex reference underflow");
    const auto retained=old_references-static_cast<std::uint32_t>(delta.removed);
    const auto references=retained+static_cast<std::uint32_t>(delta.added);
    if(references>0U){
      auto vertex=exists?old_vertex->vertex:delta.vertex;
      if(delta.added>0){
        if(retained>0U){
          const auto& first=vertex.position;const auto& second=delta.vertex.position;
          if(std::bit_cast<std::uint64_t>(first.x)!=std::bit_cast<std::uint64_t>(second.x)||
             std::bit_cast<std::uint64_t>(first.y)!=std::bit_cast<std::uint64_t>(second.y)||
             std::bit_cast<std::uint64_t>(first.z)!=std::bit_cast<std::uint64_t>(second.z))
            throw std::logic_error("blocked surface patches disagree on a shared vertex");
        }else vertex=delta.vertex;
      }
      next_vertices.push_back({vertex,references});
    }
    if(exists)++old_vertex;
  }
  next_vertices.insert(next_vertices.end(),old_vertex,
                       cache.assembled_vertices.end());
  if(stable_mode){
    if(current_to_stable.size()!=next_vertices.size())
      throw std::logic_error("stable surface identity map has the wrong size");
    for(std::size_t current=0;current<next_vertices.size();++current)
      if(current_to_stable[current]>=stable_keys.size()||
         stable_keys[current_to_stable[current]]!=
             next_vertices[current].vertex.key)
        throw std::logic_error("stable surface identity map disagrees with vertices");
  }

  const auto triangle_less=[](
      const std::array<tetra::WorldDerivedVertexKey,3>& first_vertices,
      tetra::WorldTetAddress first_owner,
      const std::array<tetra::WorldDerivedVertexKey,3>& second_vertices,
      tetra::WorldTetAddress second_owner){
    return first_vertices<second_vertices||
        (first_vertices==second_vertices&&first_owner<second_owner);
  };
  const auto triangle_equal=[](
      const std::array<tetra::WorldDerivedVertexKey,3>& first_vertices,
      tetra::WorldTetAddress first_owner,
      const std::array<tetra::WorldDerivedVertexKey,3>& second_vertices,
      tetra::WorldTetAddress second_owner){
    return first_vertices==second_vertices&&first_owner==second_owner;
  };
  std::ranges::sort(triangle_deltas,[&](const auto& first,const auto& second){
    return triangle_less(first.canonical_vertices,first.triangle.owner,
                         second.canonical_vertices,second.triangle.owner);
  });
  std::vector<TriangleDelta> grouped_triangles;
  for(const auto& delta:triangle_deltas){
    if(grouped_triangles.empty()||
       !triangle_equal(grouped_triangles.back().canonical_vertices,
           grouped_triangles.back().triangle.owner,delta.canonical_vertices,
           delta.triangle.owner))
      grouped_triangles.push_back(delta);
    else{
      grouped_triangles.back().removed+=delta.removed;
      grouped_triangles.back().added+=delta.added;
    }
  }
  next_triangles.clear();
  next_triangles.reserve(cache.assembled_triangles.size()+grouped_triangles.size());
  const auto expand_triangle=[](
      const SparseWorldSurfaceCache::CountedSurfaceTriangle& compact,
      const auto& vertices,
      std::span<const tetra::WorldDerivedVertexKey> identities){
    tetra::WorldSurfaceTriangle triangle;triangle.owner=compact.owner;
    for(std::size_t corner=0;corner<3U;++corner){
      if(!identities.empty()){
        if(compact.vertices[corner]>=identities.size())
          throw std::logic_error("stable surface triangle vertex is invalid");
        triangle.vertices[corner]=identities[compact.vertices[corner]];
      }else{
        if(compact.vertices[corner]>=vertices.size())
          throw std::logic_error("incremental surface triangle vertex is invalid");
        triangle.vertices[corner]=vertices[compact.vertices[corner]].vertex.key;
      }
    }
    return triangle;
  };
  const auto append_compact=[&](const tetra::WorldSurfaceTriangle& triangle,
                                std::uint32_t references){
    SparseWorldSurfaceCache::CountedSurfaceTriangle compact;
    compact.owner=triangle.owner;compact.references=references;
    for(std::size_t corner=0;corner<3U;++corner){
      std::size_t index{};
      if(stable_mode){
        if(current_index==nullptr)
          throw std::logic_error("stable surface identity has no current index");
        const auto found=current_index->find(triangle.vertices[corner]);
        if(found==current_index->end())
          throw std::logic_error("incremental surface triangle has no vertex");
        index=found->second;
      }else{
        const auto found=std::ranges::lower_bound(
            next_vertices,triangle.vertices[corner],{},
            [](const auto& vertex){return vertex.vertex.key;});
        if(found==next_vertices.end()||
           found->vertex.key!=triangle.vertices[corner])
          throw std::logic_error("incremental surface triangle has no vertex");
        index=static_cast<std::size_t>(found-next_vertices.begin());
      }
      const auto identity=stable_mode?current_to_stable[index]:index;
      if(identity>std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("incremental surface exceeds 32-bit vertices");
      compact.vertices[corner]=static_cast<std::uint32_t>(identity);
    }
    next_triangles.push_back(compact);
  };
  std::vector<std::uint32_t> retained_vertex_remap;
  if(!stable_mode){
    retained_vertex_remap.resize(cache.assembled_vertices.size());
    std::size_t next_vertex_index{};
    for(std::size_t old_index=0;old_index<cache.assembled_vertices.size();
        ++old_index){
      const auto& key=cache.assembled_vertices[old_index].vertex.key;
      while(next_vertex_index<next_vertices.size()&&
            next_vertices[next_vertex_index].vertex.key<key)++next_vertex_index;
      if(next_vertex_index<next_vertices.size()&&
         next_vertices[next_vertex_index].vertex.key==key){
        if(next_vertex_index>std::numeric_limits<std::uint32_t>::max())
          throw std::overflow_error("incremental surface exceeds 32-bit vertices");
        retained_vertex_remap[old_index]=
            static_cast<std::uint32_t>(next_vertex_index);
      }else retained_vertex_remap[old_index]=
          std::numeric_limits<std::uint32_t>::max();
    }
  }
  const auto append_retained_compact=[&](
      const SparseWorldSurfaceCache::CountedSurfaceTriangle& old){
    auto compact=old;
    if(stable_mode){next_triangles.push_back(compact);return;}
    for(auto& vertex:compact.vertices){
      if(vertex>=retained_vertex_remap.size()||
         retained_vertex_remap[vertex]==std::numeric_limits<std::uint32_t>::max())
        throw std::logic_error("retained surface triangle lost its vertex");
      vertex=retained_vertex_remap[vertex];
    }
    next_triangles.push_back(compact);
  };
  auto old_triangle=cache.assembled_triangles.begin();
  for(const auto& delta:grouped_triangles){
    while(old_triangle!=cache.assembled_triangles.end()){
      const auto expanded=expand_triangle(
          *old_triangle,cache.assembled_vertices,
          stable_mode?std::span(cache.optimizer_stable_keys):
                      std::span<const tetra::WorldDerivedVertexKey>{});
      const auto canonical=expanded.canonical_vertices();
      if(!triangle_less(canonical,expanded.owner,delta.canonical_vertices,
                        delta.triangle.owner))break;
      append_retained_compact(*old_triangle);++old_triangle;
    }
    const bool exists=old_triangle!=cache.assembled_triangles.end()&&
        [&]{
          const auto expanded=expand_triangle(
              *old_triangle,cache.assembled_vertices,
            stable_mode?std::span(cache.optimizer_stable_keys):
                        std::span<const tetra::WorldDerivedVertexKey>{});
          return triangle_equal(expanded.canonical_vertices(),expanded.owner,
              delta.canonical_vertices,delta.triangle.owner);
        }();
    const auto old_references=exists?old_triangle->references:0U;
    if(delta.removed<0||delta.added<0||
       static_cast<std::uint32_t>(delta.removed)>old_references)
      throw std::logic_error("incremental surface triangle reference underflow");
    const auto references=old_references-static_cast<std::uint32_t>(delta.removed)+
        static_cast<std::uint32_t>(delta.added);
    if(references>1U)
      throw std::logic_error("blocked surface publishes a duplicate triangle");
    if(references==1U)append_compact(delta.triangle,references);
    if(exists)++old_triangle;
  }
  while(old_triangle!=cache.assembled_triangles.end()){
    append_retained_compact(*old_triangle);
    ++old_triangle;
  }

  BlockedDerivedSurfaceBuild result;result.snapshots=std::move(snapshots);
  if(assemble_flat_output){
    result.vertices.reserve(next_vertices.size());
    for(const auto& vertex:next_vertices)result.vertices.push_back(vertex.vertex);
    result.triangles.reserve(next_triangles.size());
    for(const auto& triangle:next_triangles){
      tetra::WorldSurfaceTriangle expanded;expanded.owner=triangle.owner;
      for(std::size_t corner=0;corner<3U;++corner)
        expanded.vertices[corner]=stable_mode?
            stable_keys[triangle.vertices[corner]]:
            next_vertices[triangle.vertices[corner]].vertex.key;
      result.triangles.push_back(expanded);
    }
  }
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  std::uint64_t hash=offset;
  const auto append=[&](std::uint64_t value){hash^=value;hash*=prime;};
  const auto append_key=[&](const tetra::WorldDerivedVertexKey& key){
    append(static_cast<std::uint8_t>(key.kind));append(key.basis_count);
    for(std::size_t index=0;index<key.basis_count;++index){
      append(static_cast<std::uint64_t>(key.basis[index].x));
      append(static_cast<std::uint64_t>(key.basis[index].y));
      append(static_cast<std::uint64_t>(key.basis[index].z));
      append(key.basis[index].denominator_exponent);
    }
  };
  for(const auto& counted:next_vertices){
    const auto& vertex=counted.vertex;
    append_key(vertex.key);append(std::bit_cast<std::uint64_t>(vertex.position.x));
    append(std::bit_cast<std::uint64_t>(vertex.position.y));
    append(std::bit_cast<std::uint64_t>(vertex.position.z));
  }
  for(const auto& triangle:next_triangles){
    std::array<tetra::WorldDerivedVertexKey,3> keys;
    for(std::size_t corner=0;corner<3U;++corner)
      keys[corner]=stable_mode?stable_keys[triangle.vertices[corner]]:
          next_vertices[triangle.vertices[corner]].vertex.key;
    std::ranges::sort(keys);
    for(const auto& key:keys)append_key(key);
  }
  result.canonical_surface_hash=hash;return result;
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
    for(const auto point:{a,b,c})scene.triangle_vertices.push_back(
        make_scene_vertex(scene,point,{0.70F,0.36F,0.82F},normal));
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
          SceneVertex vertex=make_scene_vertex(scene,point,colour,normal);
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
        SceneVertex vertex=make_scene_vertex(scene,point,colour,normal);
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
      colour={0.05F,0.17F,0.15F};
      priority=3;
    }else if(volume_face&&vertices[0].colour[0]>0.7F){
      colour={0.18F,0.055F,0.015F};
      priority=2;
    }else{
      colour={0.045F,0.13F,0.16F};
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

BlockedDerivedSurfaceBuild assemble_blocked_derived_surface(
    const tetra::WorldCutDirectory& directory) {
  std::vector<tetra::WorldDerivedSurfaceSnapshot> snapshots;
  snapshots.reserve(directory.derived_surfaces().size());
  for(const auto& snapshot:directory.derived_surfaces())
    snapshots.push_back(*snapshot);
  auto result=assemble_blocked_snapshots(std::move(snapshots));
  result.metrics.block_generations=directory.block_generations();
  result.metrics.surface_blocks=result.snapshots.size();
  result.metrics.source_vertices=result.vertices.size();
  result.metrics.source_triangles=result.triangles.size();
  for(const auto& snapshot:result.snapshots){
    result.metrics.total_core_vertices+=snapshot.vertices.size();
    result.metrics.dependency_block_references+=snapshot.dependency_blocks.size();
    result.metrics.maximum_dependency_blocks=std::max(
        result.metrics.maximum_dependency_blocks,snapshot.dependency_blocks.size());
  }
  return result;
}

PreparedScene prepare_blocked_derived_surface_scene(
    const BlockedDerivedSurfaceBuild& surface,const tetra::Sphere& field,
    bool show_faces,bool show_edges,tetra::Vec3 render_origin) {
  PreparedScene scene;
  scene.render_origin=render_origin;
  scene.connected_surface_hash=surface.canonical_surface_hash;
  scene.optimized_surface_vertices=surface.vertices.size();
  if(!show_faces&&!show_edges)return scene;
  // The planetary renderer normally consumes only the derived boundary, not
  // the conforming volume behind it. Split that draw surface once so orbital
  // wireframes remain legible without forcing four times as many sparse
  // tetrahedral owners into the active hierarchy. Evaluate each shared edge
  // midpoint on the same band-limited field used to extract the parent. A
  // planar split only made the wire denser while retaining the visibly coarse
  // tetrahedral interpolation that looked like unstable procedural noise.
  const bool subdivide_planet_surface=field.terrain.planet_radius>0.0;
  auto midpoint_field=field;
  if(midpoint_field.sampling_footprint>0.0)
    midpoint_field.sampling_footprint*=0.5;
  scene.triangle_vertices.reserve(
      surface.triangles.size()*(subdivide_planet_surface?12U:3U));
  constexpr std::array<std::array<float,3>,3> barycentric{{
      {{1.0F,0.0F,0.0F}},{{0.0F,1.0F,0.0F}},{{0.0F,0.0F,1.0F}}}};
  for(const auto& triangle:surface.triangles){
    std::array<tetra::Vec3,3> points{};
    for(std::size_t corner=0;corner<3U;++corner){
      const auto vertex=std::ranges::lower_bound(
          surface.vertices,triangle.vertices[corner],{},
          &tetra::WorldSurfaceVertex::key);
      if(vertex==surface.vertices.end()||vertex->key!=triangle.vertices[corner])
        throw std::logic_error("blocked render triangle references a missing vertex");
      points[corner]=vertex->position;
    }
    const auto emit=[&](const std::array<tetra::Vec3,3>& render_triangle){
      auto normal=face_normal(
          render_triangle[0],render_triangle[1],render_triangle[2]);
      const auto centre=(render_triangle[0]+render_triangle[1]+
                         render_triangle[2])/3.0;
      const auto outward=field.normal(centre);
      if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0)
        normal={-normal.x,-normal.y,-normal.z};
      for(std::size_t corner=0;corner<3U;++corner){
        SceneVertex vertex{};
        const auto position=render_position(scene,render_triangle[corner]);
        std::ranges::copy(position,vertex.position);
        vertex.colour[0]=0.24F;vertex.colour[1]=0.76F;vertex.colour[2]=0.38F;
        vertex.normal[0]=static_cast<float>(normal.x);
        vertex.normal[1]=static_cast<float>(normal.y);
        vertex.normal[2]=static_cast<float>(normal.z);
        vertex.diagnostics[0]=-2.0F;
        vertex.edge_flags=show_edges?7.0F:0.0F;
        std::ranges::copy(barycentric[corner],vertex.barycentric);
        scene.triangle_vertices.push_back(vertex);
      }
    };
    const auto subdivide=[&](auto&& self,
                             const std::array<tetra::Vec3,3>& source,
                             unsigned int levels)->void{
      if(levels==0U){emit(source);return;}
      const auto midpoint=[&](tetra::Vec3 first,tetra::Vec3 second){
        const auto centre=(first+second)/2.0;
        return subdivide_planet_surface?
            midpoint_field.project_to_surface(centre):centre;
      };
      const auto ab=midpoint(source[0],source[1]);
      const auto bc=midpoint(source[1],source[2]);
      const auto ca=midpoint(source[2],source[0]);
      self(self,{source[0],ab,ca},levels-1U);
      self(self,{ab,source[1],bc},levels-1U);
      self(self,{ca,bc,source[2]},levels-1U);
      self(self,{ab,bc,ca},levels-1U);
    };
    subdivide(subdivide,points,subdivide_planet_surface?1U:0U);
  }
  // Keep lighting independent of triangle scale. The blocked path used to
  // submit raw area vectors here; sufficiently refined faces then fell below
  // the fragment shader's zero-normal sentinel and appeared as isolated,
  // fully bright triangles. This is the same flat-face attribute preparation
  // used by the monolithic scene path.
  prepare_surface_render_attributes(scene,&field);
  append_screen_space_edges(scene,show_edges,false,show_faces,false);
  scene.connected_surface_edges=scene.surface_line_vertices.size()/2U;
  return scene;
}

RetainedPreparedSceneBuild prepare_retained_blocked_scene(
    const BlockedDerivedSurfaceBuild& surface,const tetra::Sphere& field,
    bool show_faces,bool show_edges,tetra::Vec3 render_origin,
    SparseWorldSurfaceCache& cache,bool assemble_flat_scene) {
  const auto payload_hash=[](const tetra::WorldDerivedSurfaceSnapshot& snapshot){
    constexpr std::uint64_t offset=1469598103934665603ULL;
    constexpr std::uint64_t prime=1099511628211ULL;
    std::uint64_t hash=offset;
    const auto add=[&](std::uint64_t value){hash^=value;hash*=prime;};
    const auto add_key=[&](const tetra::WorldDerivedVertexKey& key){
      add(static_cast<std::uint8_t>(key.kind));add(key.basis_count);
      for(std::size_t basis=0;basis<key.basis_count;++basis){
        add(static_cast<std::uint64_t>(key.basis[basis].x));
        add(static_cast<std::uint64_t>(key.basis[basis].y));
        add(static_cast<std::uint64_t>(key.basis[basis].z));
        add(key.basis[basis].denominator_exponent);
      }
    };
    add(snapshot.id.prefix.high);add(snapshot.id.prefix.low);
    add(snapshot.id.block_generations);add(snapshot.metrics.optimizer_passes);
    add(snapshot.metrics.dependency_halo_rings);
    for(const auto& vertex:snapshot.vertices){
      add_key(vertex.key);add(std::bit_cast<std::uint64_t>(vertex.position.x));
      add(std::bit_cast<std::uint64_t>(vertex.position.y));
      add(std::bit_cast<std::uint64_t>(vertex.position.z));
    }
    for(const auto& triangle:snapshot.triangles){
      for(const auto& key:triangle.vertices)add_key(key);
      add(triangle.owner.high);add(triangle.owner.low);
    }
    return hash;
  };

  RetainedPreparedSceneBuild result;
  result.scene.render_origin=render_origin;
  result.scene.connected_surface_hash=surface.canonical_surface_hash;
  result.scene.optimized_surface_vertices=surface.metrics.source_vertices;
  std::vector<SparseWorldSurfaceCache::RenderBlock> next;
  next.reserve(surface.snapshots.size());
  if(assemble_flat_scene)
    result.scene.triangle_vertices.reserve(surface.metrics.source_triangles*3U);
  for(const auto& snapshot:surface.snapshots){
    const auto signature=payload_hash(snapshot);
    const auto found=std::ranges::lower_bound(cache.render_blocks,snapshot.id,{},
        &SparseWorldSurfaceCache::RenderBlock::id);
    const bool same_origin=found!=cache.render_blocks.end()&&
        found->render_origin.x==render_origin.x&&
        found->render_origin.y==render_origin.y&&
        found->render_origin.z==render_origin.z;
    if(found!=cache.render_blocks.end()&&found->id==snapshot.id&&
       found->surface_payload_hash==signature&&same_origin&&
       found->show_faces==show_faces&&found->show_edges==show_edges){
      // Each cached block is consumed once in sorted order and the cache is
      // replaced below. Move its immutable vertex allocation into the next
      // front instead of copying every retained block during publication.
      next.push_back(std::move(*found));++result.reused_blocks;
    }else{
      BlockedDerivedSurfaceBuild block;
      block.snapshots.push_back(snapshot);block.vertices=snapshot.vertices;
      block.triangles=snapshot.triangles;
      auto prepared=prepare_blocked_derived_surface_scene(
          block,field,show_faces,show_edges,render_origin);
      SparseWorldSurfaceCache::RenderBlock rendered;
      rendered.id=snapshot.id;rendered.surface_payload_hash=signature;
      rendered.render_origin=render_origin;rendered.show_faces=show_faces;
      rendered.show_edges=show_edges;
      rendered.triangle_vertices=std::move(prepared.triangle_vertices);
      next.push_back(std::move(rendered));++result.rebuilt_blocks;
    }
    if(assemble_flat_scene){
      const auto& rendered=next.back().triangle_vertices;
      result.scene.triangle_vertices.insert(result.scene.triangle_vertices.end(),
          rendered.begin(),rendered.end());
    }
  }
  cache.render_blocks=std::move(next);
  if(assemble_flat_scene){
    append_screen_space_edges(result.scene,show_edges,false,show_faces,false);
    result.scene.connected_surface_edges=
        result.scene.surface_line_vertices.size()/2U;
  }
  return result;
}

BlockedDerivedSurfaceBuild build_sparse_world_derived_surface(
    const tetra::WorldCutDirectory& directory,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Sphere& field,bool optimize,
    std::stop_token cancellation,SparseWorldSurfaceCache* cache,
    std::span<const tetra::HierarchyBlockId> retained_volume_blocks,
    bool restrict_retained_volume,bool compute_complete_volume_oracle,
    std::span<const tetra::HierarchyBlockId> changed_hierarchy_blocks,
    tetra::GeometryExecutor* executor,bool assemble_flat_output) {
  const auto started=std::chrono::steady_clock::now();
  if(!(domain.world_extent>0.0)||!std::isfinite(domain.world_extent))
    throw std::invalid_argument("sparse world surface requires a finite domain");
  if(!std::ranges::is_sorted(retained_volume_blocks)||
     std::ranges::adjacent_find(retained_volume_blocks)!=
         retained_volume_blocks.end())
    throw std::invalid_argument("retained world volume block set is not canonical");
  if(!std::ranges::is_sorted(changed_hierarchy_blocks)||
     std::ranges::adjacent_find(changed_hierarchy_blocks)!=
         changed_hierarchy_blocks.end())
    throw std::invalid_argument("changed hierarchy block set is not canonical");
  constexpr std::uint64_t field_hash_offset=1469598103934665603ULL;
  constexpr std::uint64_t field_hash_prime=1099511628211ULL;
  std::uint64_t field_signature=field_hash_offset;
  const auto hash_field_value=[&](const auto& value){
    const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
    for(std::size_t index=0;index<sizeof(value);++index){
      field_signature^=bytes[index];field_signature*=field_hash_prime;
    }
  };
  hash_field_value(field.kind);
  for(const double value:{field.centre.x,field.centre.y,field.centre.z,
      field.radius,field.secondary,field.frequency,field.sampling_footprint,
      field.terrain.height_offset,field.terrain.landform_amplitude,
      field.terrain.landform_frequency,field.terrain.mountain_amplitude,
      field.terrain.mountain_ridge_frequency,
      field.terrain.mountain_range_frequency,
      field.terrain.gameplay_hill_amplitude,
      field.terrain.gameplay_hill_frequency,
      field.terrain.gameplay_feature_amplitude,
      field.terrain.gameplay_feature_frequency,
      field.terrain.gameplay_region_frequency,
      field.terrain.gameplay_corridor_depth,
      field.terrain.gameplay_warp_amplitude,
      field.terrain.gameplay_warp_frequency,
      field.terrain.ground_roughness_amplitude,
      field.terrain.ground_roughness_frequency,
      field.terrain.spawn_flat_radius,field.terrain.spawn_blend_radius,
      field.terrain.planet_radius,
      domain.world_origin.x,domain.world_origin.y,domain.world_origin.z,
      domain.world_extent})hash_field_value(value);
  const bool field_changed=cache&&cache->surface_field_signature!=0U&&
      cache->surface_field_signature!=field_signature;
  const bool closure_changes_unconsumed=cache&&
      cache->surface_source_hierarchy_revision!=directory.revision();
  const bool closure_directory_certified=cache&&
      cache->closure_source_hierarchy_revision==directory.revision();
  if(cache){
    const bool certified=closure_directory_certified;
    if(!certified){
      std::vector<tetra::WorldTetAddress> owners;
      owners.reserve(directory.logical_owner_count());
      directory.for_each_logical_owner(
          [&](tetra::WorldTetAddress owner){owners.push_back(owner);});
      std::ranges::sort(owners);
      if(cache->closure.closed_owners!=owners||
         cache->closure.green_masks.size()!=owners.size()){
        const auto closed=tetra::close_world_conforming_cut(
            owners,&cache->closure,cancellation,directory.block_generations());
        if(closed!=owners)
          throw std::logic_error(
              "sparse surface directory is missing conforming closure owners");
      }
      cache->closure_source_hierarchy_revision=directory.revision();
    }else if(cache->closure.closed_owners.size()!=
                 directory.logical_owner_count()||
             cache->closure.green_masks.size()!=
                 cache->closure.closed_owners.size()){
      throw std::logic_error(
          "certified sparse surface closure has inconsistent owner counts");
    }
  }
  const auto previous_conforming_blocks=cache?cache->conforming.blocks:
      std::vector<std::shared_ptr<const tetra::WorldConformingBlockSnapshot>>{};
  const auto topology_payload_hash=[](
      const tetra::HierarchyBlockSnapshot& block){
    constexpr std::uint64_t offset=1469598103934665603ULL;
    constexpr std::uint64_t prime=1099511628211ULL;
    std::uint64_t hash=offset;
    const auto add=[&](const void* value,std::size_t size){
      const auto* bytes=static_cast<const unsigned char*>(value);
      for(std::size_t index=0;index<size;++index){hash^=bytes[index];hash*=prime;}
    };
    add(&block.id,sizeof(block.id));
    for(const auto owner:block.logical_owners)add(&owner,sizeof(owner));
    for(const auto resident:block.resident_records)add(&resident,sizeof(resident));
    return hash;
  };
  const auto block_hash=[](const tetra::HierarchyBlockId& id){
    std::uint64_t hash=id.prefix.high*0x9e3779b97f4a7c15ULL;
    hash^=id.prefix.low+0x9e3779b97f4a7c15ULL+(hash<<6U)+(hash>>2U);
    hash^=id.block_generations+0x9e3779b97f4a7c15ULL+(hash<<6U)+(hash>>2U);
    return static_cast<std::size_t>(hash);
  };
  std::unordered_set<tetra::HierarchyBlockId,decltype(block_hash)>
      topology_current_blocks(0U,block_hash),
      hierarchy_payload_changed_blocks(0U,block_hash);
  topology_current_blocks.reserve(directory.hierarchy_blocks().size());
  hierarchy_payload_changed_blocks.reserve(
      changed_hierarchy_blocks.size()+1U);
  std::vector<SparseWorldSurfaceCache::HierarchySignature> hierarchy;
  hierarchy.reserve(directory.hierarchy_blocks().size());
  for(const auto& block:directory.hierarchy_blocks()){
    topology_current_blocks.insert(block->id);
    const auto previous=cache?std::ranges::lower_bound(
        cache->hierarchy,block->id,{},
        &SparseWorldSurfaceCache::HierarchySignature::id):
        std::vector<SparseWorldSurfaceCache::HierarchySignature>::const_iterator{};
    const bool manifest_changed=!closure_directory_certified||
        std::ranges::binary_search(changed_hierarchy_blocks,block->id);
    const auto payload_hash=!manifest_changed&&previous!=cache->hierarchy.end()&&
            previous->id==block->id?
        previous->hash:topology_payload_hash(*block);
    hierarchy.push_back({block->id,payload_hash});
    if(!cache||field_changed||previous==cache->hierarchy.end()||
       previous->id!=block->id||previous->hash!=payload_hash)
      hierarchy_payload_changed_blocks.insert(block->id);
  }
  std::unordered_set<tetra::HierarchyBlockId,decltype(block_hash)>
      active_owner_blocks(0U,block_hash);
  if(!cache)for(const auto& block:directory.hierarchy_blocks())
    if(!block->logical_owners.empty())active_owner_blocks.insert(block->id);
  std::unordered_set<tetra::HierarchyBlockId,decltype(block_hash)>
      surface_candidate_blocks(0U,block_hash);
  struct CandidateOwner {
    std::shared_ptr<SparseWorldSurfaceCache::SurfaceCertificateBlock> block;
    std::size_t certificate{};
  };
  std::vector<CandidateOwner> surface_candidate_owner_records;
  std::vector<std::shared_ptr<
      SparseWorldSurfaceCache::SurfaceCertificateBlock>> certificate_blocks;
  std::size_t surface_candidate_owners{},surface_classification_samples{};
  std::size_t reused_surface_certificates{},rebuilt_surface_certificates{};
  std::unordered_set<tetra::HierarchyBlockId,decltype(block_hash)>
      certificate_changed_blocks(0U,block_hash);
  const double field_lipschitz=tetra::implicit_field_lipschitz_bound(field);
  if(cache){
  const auto classify=[&](tetra::WorldTetAddress owner,std::uint8_t green_mask){
    const auto geometry=std::ranges::lower_bound(
        cache->closure.geometry,owner,{},
        &tetra::WorldConformingClosureCacheEntry::address);
    const auto keys=geometry!=cache->closure.geometry.end()&&
            geometry->address==owner?geometry->vertices:
        tetra::world_tetrahedron_vertex_keys(owner);
    std::array<tetra::Vec3,4> points{};
    tetra::Vec3 centre{};
    bool negative{},positive{};
    double minimum_y=std::numeric_limits<double>::infinity();
    double maximum_y=-std::numeric_limits<double>::infinity();
    for(std::size_t corner=0;corner<points.size();++corner){
      const tetra::Vec3 root{
          std::ldexp(static_cast<double>(keys[corner].x),
                     -keys[corner].denominator_exponent),
          std::ldexp(static_cast<double>(keys[corner].y),
                     -keys[corner].denominator_exponent),
          std::ldexp(static_cast<double>(keys[corner].z),
                     -keys[corner].denominator_exponent)};
      points[corner]=domain.to_world(root);centre=centre+points[corner];
      const double distance=field.signed_distance(points[corner]);
      ++surface_classification_samples;
      negative|=distance<0.0;positive|=distance>=0.0;
      minimum_y=std::min(minimum_y,points[corner].y);
      maximum_y=std::max(maximum_y,points[corner].y);
    }
    centre=centre/4.0;
    double radius{},horizontal_radius{};
    for(const auto point:points){
      const auto offset=point-centre;
      radius=std::max(radius,std::sqrt(
          offset.x*offset.x+offset.y*offset.y+offset.z*offset.z));
      horizontal_radius=std::max(horizontal_radius,
          std::hypot(offset.x,offset.z));
    }
    bool may_cross=negative&&positive;
    if(!may_cross&&field.kind==tetra::ImplicitShapeKind::perlin_terrain&&
       !(field.terrain.planet_radius>0.0)){
      const double height=tetra::terrain_height_sample(
          field,centre.x,centre.z).height;
      ++surface_classification_samples;
      const double uncertainty=tetra::terrain_height_slope_bound(
          field,centre.x,centre.z,horizontal_radius)*horizontal_radius;
      may_cross=minimum_y<=height+uncertainty&&
          maximum_y>=height-uncertainty;
    }else if(!may_cross){
      may_cross=std::abs(field.signed_distance(centre))<=
          field_lipschitz*radius;
      ++surface_classification_samples;
    }
    return SparseWorldSurfaceCache::SurfaceOwnerCertificate{
        .owner=owner,.green_mask=green_mask,.may_cross=may_cross};
  };
  std::size_t dependency_begin{},previous_block_index{};
  while(dependency_begin<cache->closure.dependency_blocks.size()){
    if(cancellation.stop_requested())
      throw std::runtime_error("sparse world surface classification canceled");
    const auto block_id=cache->closure.dependency_blocks[dependency_begin]->id;
    std::size_t dependency_end=dependency_begin+1U;
    while(dependency_end<cache->closure.dependency_blocks.size()&&
          cache->closure.dependency_blocks[dependency_end]->id==block_id)
      ++dependency_end;
    while(previous_block_index<cache->surface_certificate_blocks.size()&&
          cache->surface_certificate_blocks[previous_block_index]->id<block_id)
      ++previous_block_index;
    const auto previous=previous_block_index<
            cache->surface_certificate_blocks.size()&&
            cache->surface_certificate_blocks[previous_block_index]->id==block_id?
        cache->surface_certificate_blocks[previous_block_index]:nullptr;
    const bool dirty=field_changed||!previous||
        hierarchy_payload_changed_blocks.contains(block_id)||
        (closure_changes_unconsumed&&std::ranges::binary_search(
            cache->closure.last_changed_mask_blocks,block_id));
    if(!dirty){
      certificate_blocks.push_back(previous);
      reused_surface_certificates+=previous->certificates.size();
      surface_candidate_owners+=previous->candidate_owners;
      if(previous->candidate_owners>0U)
        surface_candidate_blocks.insert(block_id);
      dependency_begin=dependency_end;continue;
    }
    auto block=std::make_shared<
        SparseWorldSurfaceCache::SurfaceCertificateBlock>();
    block->id=block_id;certificate_changed_blocks.insert(block_id);
    std::size_t owner_count{};
    for(std::size_t run=dependency_begin;run<dependency_end;++run)
      owner_count+=cache->closure.dependency_blocks[run]->owners.size();
    block->certificates.reserve(owner_count);
    std::vector<tetra::WorldTetAddress> block_owners;
    block_owners.reserve(owner_count);
    for(std::size_t run=dependency_begin;run<dependency_end;++run)
      block_owners.insert(block_owners.end(),
          cache->closure.dependency_blocks[run]->owners.begin(),
          cache->closure.dependency_blocks[run]->owners.end());
    std::ranges::sort(block_owners);
    for(const auto owner:block_owners){
      const auto mask_owner=std::ranges::lower_bound(
          cache->closure.closed_owners,owner);
      if(mask_owner==cache->closure.closed_owners.end()||*mask_owner!=owner)
        throw std::logic_error("certificate block owner is absent from closure");
      const auto mask_index=static_cast<std::size_t>(
          mask_owner-cache->closure.closed_owners.begin());
      const auto green_mask=cache->closure.green_masks[mask_index];
      const auto old=previous?std::ranges::lower_bound(
          previous->certificates,owner,{},
          &SparseWorldSurfaceCache::SurfaceOwnerCertificate::owner):
          std::vector<SparseWorldSurfaceCache::SurfaceOwnerCertificate>::iterator{};
      const bool reusable=previous&&cache->surface_field_signature==field_signature&&
          old!=previous->certificates.end()&&old->owner==owner&&
          old->green_mask==green_mask;
      block->certificates.push_back(reusable?*old:classify(owner,green_mask));
      reusable?++reused_surface_certificates:++rebuilt_surface_certificates;
      if(block->certificates.back().may_cross){
        ++block->candidate_owners;++surface_candidate_owners;
        surface_candidate_owner_records.push_back(
            {block,block->certificates.size()-1U});
      }
    }
    if(block->candidate_owners>0U)surface_candidate_blocks.insert(block_id);
    certificate_blocks.push_back(std::move(block));
    dependency_begin=dependency_end;
  }
  }else{
    surface_candidate_blocks.insert(active_owner_blocks.begin(),
                                    active_owner_blocks.end());
    surface_candidate_owners=directory.logical_owner_count();
  }
  const auto classified=std::chrono::steady_clock::now();
  std::vector<tetra::HierarchyBlockId> materialized_blocks(
      retained_volume_blocks.begin(),retained_volume_blocks.end());
  std::ranges::sort(materialized_blocks);
  materialized_blocks.erase(std::unique(materialized_blocks.begin(),
      materialized_blocks.end()),materialized_blocks.end());
  tetra::WorldBlockedConformingVolume volume;
  if(cache){
    volume=tetra::reconstruct_blocked_world_conforming_volume(
        directory,cache->closure,&cache->conforming,materialized_blocks,
        restrict_retained_volume,compute_complete_volume_oracle);
  }else{
    const auto flat=tetra::reconstruct_world_conforming_volume(directory);
    volume.logical_owners=flat.logical_owners;
    volume.transition_cells=flat.transition_cells;
    volume.cells=flat.cells.size();volume.rebuilt_cells=flat.cells.size();
    std::size_t begin{};
    while(begin<flat.cells.size()){
      auto block=std::make_shared<tetra::WorldConformingBlockSnapshot>();
      block->id=tetra::hierarchy_block_id(
          flat.cells[begin].logical_owner,directory.block_generations());
      std::size_t end=begin+1U;
      while(end<flat.cells.size()&&tetra::hierarchy_block_id(
            flat.cells[end].logical_owner,directory.block_generations())==
            block->id)++end;
      block->cells.assign(flat.cells.begin()+static_cast<std::ptrdiff_t>(begin),
                          flat.cells.begin()+static_cast<std::ptrdiff_t>(end));
      bool first_owner=true;
      tetra::WorldTetAddress previous_owner{};
      for(const auto& cell:block->cells)
        if(first_owner||cell.logical_owner!=previous_owner){
          ++block->logical_owners;previous_owner=cell.logical_owner;
          first_owner=false;
        }
      volume.blocks.push_back(std::move(block));++volume.rebuilt_blocks;
      begin=end;
    }
  }
  if(!compute_complete_volume_oracle){
    constexpr std::uint64_t offset=1469598103934665603ULL;
    constexpr std::uint64_t prime=1099511628211ULL;
    volume.canonical_hash=offset;
    const auto add=[&](const void* value,std::size_t size){
      const auto* bytes=static_cast<const unsigned char*>(value);
      for(std::size_t index=0;index<size;++index){
        volume.canonical_hash^=bytes[index];volume.canonical_hash*=prime;
      }
    };
    for(std::size_t index=0;index<cache->closure.closed_owners.size();++index){
      add(&cache->closure.closed_owners[index],
          sizeof(cache->closure.closed_owners[index]));
      add(&cache->closure.green_masks[index],
          sizeof(cache->closure.green_masks[index]));
    }
  }else if(volume.canonical_hash==0U){
    constexpr std::uint64_t offset=1469598103934665603ULL;
    constexpr std::uint64_t prime=1099511628211ULL;
    volume.canonical_hash=offset;
    const auto add=[&](const void* value,std::size_t size){
      const auto* bytes=static_cast<const unsigned char*>(value);
      for(std::size_t index=0;index<size;++index){
        volume.canonical_hash^=bytes[index];volume.canonical_hash*=prime;
      }
    };
    for(const auto& block:volume.blocks)for(const auto& cell:block->cells){
      auto keys=cell.vertices;std::ranges::sort(keys);
      add(&cell.logical_owner,sizeof(cell.logical_owner));
      add(keys.data(),sizeof(keys));
    }
  }
  const auto volume_reconstructed=std::chrono::steady_clock::now();
  const std::uint64_t conforming_volume_hash=volume.canonical_hash;
  auto topology_changed_blocks=hierarchy_payload_changed_blocks;
  if(!cache||field_changed){
    topology_changed_blocks.clear();
    topology_changed_blocks.insert(topology_current_blocks.begin(),
                                   topology_current_blocks.end());
  }
  else if(closure_changes_unconsumed)
    topology_changed_blocks.insert(
        cache->closure.last_changed_mask_blocks.begin(),
        cache->closure.last_changed_mask_blocks.end());
  // Certificate changes include field-revision invalidation and provide a
  // redundant exact check for callers that populate closure state externally.
  topology_changed_blocks.insert(certificate_changed_blocks.begin(),
                                 certificate_changed_blocks.end());
  struct KeyTriangle {
    std::array<tetra::WorldDerivedVertexKey,3> vertices{};
    tetra::WorldTetAddress owner{};
  };
  std::map<tetra::WorldDerivedVertexKey,tetra::Vec3> all_surface_vertices;
  std::vector<KeyTriangle> all_surface_triangles;
  std::size_t direct_green_cells_enumerated{};
  std::size_t reused_intersections{},computed_intersections{};
  constexpr std::array<std::array<std::size_t,2>,6> surface_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  const auto cross=[](tetra::Vec3 a,tetra::Vec3 b){return tetra::Vec3{
      a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return
      a.x*b.x+a.y*b.y+a.z*b.z;};
  bool retained_raw_complete=cache&&
      cache->raw_blocks.size()==cache->snapshots.size()&&
      cache->intersection_references.size()==cache->intersections.size();
  if(retained_raw_complete)
    for(std::size_t block=0;block<cache->snapshots.size();++block)
      retained_raw_complete=retained_raw_complete&&
          cache->raw_blocks[block]->id==cache->snapshots[block].id&&
          cache->raw_blocks[block]->vertices.size()==
              cache->snapshots[block].vertices.size()&&
          cache->raw_blocks[block]->triangles.size()==
              cache->snapshots[block].triangles.size();
  if(cache){
    if(!field_changed)for(const auto& snapshot:cache->snapshots){
      if(!topology_current_blocks.contains(snapshot.id)||
         topology_changed_blocks.contains(snapshot.id))continue;
      if(retained_raw_complete)continue;
      const auto raw_block=std::ranges::lower_bound(
          cache->raw_blocks,snapshot.id,{},
          [](const auto& block){return block->id;});
      const bool has_raw_block=raw_block!=cache->raw_blocks.end()&&
          (*raw_block)->id==snapshot.id;
      if(has_raw_block&&(*raw_block)->vertices.size()!=snapshot.vertices.size())
        throw std::logic_error(
            "retained surface raw block does not match its snapshot");
      if(has_raw_block){
        for(const auto& triangle:(*raw_block)->triangles){
          KeyTriangle expanded;expanded.owner=triangle.owner;
          for(std::size_t corner=0;corner<3U;++corner){
            if(triangle.vertices[corner]>=(*raw_block)->vertices.size())
              throw std::logic_error(
                  "retained surface raw triangle has an invalid vertex");
            expanded.vertices[corner]=
                (*raw_block)->vertices[triangle.vertices[corner]].key;
          }
          all_surface_triangles.push_back(expanded);
        }
        for(const auto& vertex:(*raw_block)->vertices)
          if(all_surface_vertices.emplace(vertex.key,vertex.position).second)
            ++reused_intersections;
      }else{
        for(const auto& triangle:snapshot.triangles)
          all_surface_triangles.push_back({triangle.vertices,triangle.owner});
        for(const auto& vertex:snapshot.vertices){
          const auto raw=std::ranges::lower_bound(
              cache->intersections,vertex.key,{},
              &tetra::WorldSurfaceVertex::key);
          if(raw==cache->intersections.end()||raw->key!=vertex.key)
            throw std::logic_error(
                "retained surface snapshot has no raw field crossing");
          if(all_surface_vertices.emplace(raw->key,raw->position).second)
            ++reused_intersections;
        }
      }
    }
    for(const auto& candidate:surface_candidate_owner_records){
      auto& certificate=candidate.block->certificates[candidate.certificate];
      const auto owner=certificate.owner;
      const auto owner_block=candidate.block->id;
      if(!topology_changed_blocks.contains(owner_block))continue;
      const auto geometry=std::ranges::lower_bound(
          cache->closure.geometry,owner,{},
          &tetra::WorldConformingClosureCacheEntry::address);
      const auto keys=geometry!=cache->closure.geometry.end()&&
              geometry->address==owner?geometry->vertices:
          tetra::world_tetrahedron_vertex_keys(owner);
      std::array<tetra::Vec3,4> owner_points{};
      for(std::size_t corner=0;corner<owner_points.size();++corner)
        owner_points[corner]={
            std::ldexp(static_cast<double>(keys[corner].x),
                       -keys[corner].denominator_exponent),
            std::ldexp(static_cast<double>(keys[corner].y),
                       -keys[corner].denominator_exponent),
            std::ldexp(static_cast<double>(keys[corner].z),
                       -keys[corner].denominator_exponent)};
      const auto& green=tetra::complete_green_template(
          certificate.green_mask);
      std::array<tetra::Vec3,10> root_points{},world_points{};
      std::array<tetra::WorldVertexKey,10> point_keys{};
      std::array<double,10> distances{};
      for(std::size_t point=0;point<root_points.size();++point){
        if(tetra::grande_point_vertex[point]!=0xffU){
          const auto vertex=tetra::grande_point_vertex[point];
          root_points[point]=owner_points[vertex];point_keys[point]=keys[vertex];
        }else{
          const auto edge=surface_edges[tetra::grande_point_edge[point]];
          root_points[point]=(owner_points[edge[0]]+owner_points[edge[1]])/2.0;
          point_keys[point]=tetra::world_vertex_key(root_points[point]);
        }
        world_points[point]=domain.to_world(root_points[point]);
        if(certificate.has_grande_signs)
          distances[point]=(certificate.negative_grande_points&(1U<<point))!=0U?
              -1.0:1.0;
        else{
          distances[point]=field.signed_distance(world_points[point]);
          if(distances[point]<0.0)
            certificate.negative_grande_points|=
                static_cast<std::uint16_t>(1U<<point);
        }
      }
      if(!certificate.has_grande_signs){
        surface_classification_samples+=root_points.size();
        certificate.has_grande_signs=true;
      }
      for(std::size_t cell_index=0;cell_index<green.count;++cell_index){
        ++direct_green_cells_enumerated;
        const auto& cell=green.tetrahedra[cell_index];
        struct Crossing {
          tetra::WorldDerivedVertexKey key{};tetra::Vec3 point{};
        };
        std::array<Crossing,4> crossings{};
        std::size_t crossing_count{};
        for(const auto edge:surface_edges){
          const auto first=cell[edge[0]],second=cell[edge[1]];
          if((distances[first]<0.0)==(distances[second]<0.0))continue;
          const auto key=tetra::world_edge_intersection_key(
              point_keys[first],point_keys[second]);
          tetra::Vec3 point;
          const auto cached=std::ranges::lower_bound(
              cache->intersections,key,{},&tetra::WorldSurfaceVertex::key);
          if(!field_changed&&cached!=cache->intersections.end()&&
             cached->key==key){
            point=cached->position;++reused_intersections;
          }else{
            const bool ordered=point_keys[first]<point_keys[second];
            point=field.edge_intersection(
                world_points[ordered?first:second],
                world_points[ordered?second:first]);
            ++computed_intersections;
          }
          crossings[crossing_count++]={key,point};
          const auto [found,inserted]=all_surface_vertices.emplace(key,point);
          if(!inserted){
            const auto delta=found->second-point;
            if(dot(delta,delta)>1.0e-20)
              throw std::logic_error(
                  "direct sparse surface disagrees on a shared crossing");
          }
        }
        if(crossing_count<3U)continue;
        tetra::Vec3 centre{};
        for(std::size_t index=0;index<crossing_count;++index)
          centre=centre+crossings[index].point;
        centre=centre/static_cast<double>(crossing_count);
        const auto normal=field.normal(centre);
        const auto reference=std::abs(normal.z)<0.9?
            tetra::Vec3{0.0,0.0,1.0}:tetra::Vec3{0.0,1.0,0.0};
        const auto axis_u=cross(reference,normal),axis_v=cross(normal,axis_u);
        std::sort(crossings.begin(),
            crossings.begin()+static_cast<std::ptrdiff_t>(crossing_count),
            [&](const Crossing& first,const Crossing& second){
              const auto a=first.point-centre,b=second.point-centre;
              return std::atan2(dot(a,axis_v),dot(a,axis_u))<
                     std::atan2(dot(b,axis_v),dot(b,axis_u));
            });
        for(std::size_t index=1U;index+1U<crossing_count;++index)
          all_surface_triangles.push_back({{{crossings[0].key,
              crossings[index].key,crossings[index+1U].key}},owner});
      }
    }
  }
  std::map<tetra::HierarchyBlockId,std::vector<KeyTriangle>>
      rebuilt_raw_triangles;
  for(const auto& triangle:all_surface_triangles)
    rebuilt_raw_triangles[tetra::hierarchy_block_id(
        triangle.owner,directory.block_generations())].push_back(triangle);
  std::vector<std::shared_ptr<const SparseWorldSurfaceCache::SurfaceRawBlock>>
      current_raw_blocks;
  if(retained_raw_complete&&!field_changed)
    for(const auto& block:cache->raw_blocks)
      if(topology_current_blocks.contains(block->id)&&
         !topology_changed_blocks.contains(block->id))
        current_raw_blocks.push_back(block);
  for(auto& [id,block_triangles]:rebuilt_raw_triangles){
    auto block=std::make_shared<SparseWorldSurfaceCache::SurfaceRawBlock>();
    block->id=id;block->source_hierarchy_revision=directory.revision();
    std::set<tetra::WorldDerivedVertexKey> block_keys;
    for(const auto& triangle:block_triangles)
      block_keys.insert(triangle.vertices.begin(),triangle.vertices.end());
    block->vertices.reserve(block_keys.size());
    for(const auto& key:block_keys){
      const auto found=all_surface_vertices.find(key);
      if(found==all_surface_vertices.end())
        throw std::logic_error("raw surface block has no field crossing");
      block->vertices.push_back({found->first,found->second});
    }
    block->triangles.reserve(block_triangles.size());
    for(const auto& triangle:block_triangles){
      SparseWorldSurfaceCache::SurfaceRawBlock::Triangle compact;
      compact.owner=triangle.owner;
      for(std::size_t corner=0;corner<3U;++corner){
        const auto found=std::ranges::lower_bound(
            block->vertices,triangle.vertices[corner],{},
            &tetra::WorldSurfaceVertex::key);
        if(found==block->vertices.end()||
           found->key!=triangle.vertices[corner])
          throw std::logic_error("raw surface triangle has no local vertex");
        compact.vertices[corner]=static_cast<std::uint32_t>(
            found-block->vertices.begin());
      }
      block->triangles.push_back(compact);
    }
    current_raw_blocks.push_back(std::move(block));
  }
  std::ranges::sort(current_raw_blocks,{},
      [](const auto& block){return block->id;});
  if(std::ranges::adjacent_find(current_raw_blocks,[](const auto& first,
                                                      const auto& second){
       return first->id==second->id;
     })!=current_raw_blocks.end())
    throw std::logic_error("surface raw arena contains duplicate blocks");

  struct RawVertexDelta {
    tetra::WorldSurfaceVertex vertex;
    std::int32_t removed{},added{};
  };
  std::vector<RawVertexDelta> raw_vertex_deltas;
  if(retained_raw_complete&&!field_changed){
    for(const auto& block:cache->raw_blocks)
      if(!topology_current_blocks.contains(block->id)||
         topology_changed_blocks.contains(block->id))
        for(const auto& vertex:block->vertices)
          raw_vertex_deltas.push_back({vertex,1,0});
    for(const auto& block:current_raw_blocks)
      if(topology_changed_blocks.contains(block->id))
        for(const auto& vertex:block->vertices)
          raw_vertex_deltas.push_back({vertex,0,1});
  }else for(const auto& block:current_raw_blocks)
    for(const auto& vertex:block->vertices)
      raw_vertex_deltas.push_back({vertex,0,1});
  std::ranges::sort(raw_vertex_deltas,[](const auto& first,const auto& second){
    return first.vertex.key<second.vertex.key;
  });
  std::vector<RawVertexDelta> grouped_raw_vertices;
  for(const auto& delta:raw_vertex_deltas){
    if(grouped_raw_vertices.empty()||
       grouped_raw_vertices.back().vertex.key!=delta.vertex.key){
      grouped_raw_vertices.push_back(delta);continue;
    }
    auto& grouped=grouped_raw_vertices.back();
    if(delta.added>0){
      const auto difference=grouped.vertex.position-delta.vertex.position;
      if(grouped.added>0&&dot(difference,difference)>1.0e-20)
        throw std::logic_error("raw surface blocks disagree on a crossing");
      grouped.vertex=delta.vertex;
    }
    grouped.removed+=delta.removed;grouped.added+=delta.added;
  }
  std::vector<tetra::WorldSurfaceVertex> next_intersections;
  std::vector<std::uint32_t> next_intersection_references;
  next_intersections.reserve(cache?cache->intersections.size():
                             grouped_raw_vertices.size());
  next_intersection_references.reserve(next_intersections.capacity());
  const bool retain_raw_directory=retained_raw_complete&&!field_changed;
  auto retained_raw_vertex=retain_raw_directory?cache->intersections.begin():
      std::vector<tetra::WorldSurfaceVertex>::const_iterator{};
  const auto retained_raw_end=retain_raw_directory?cache->intersections.end():
      std::vector<tetra::WorldSurfaceVertex>::const_iterator{};
  std::size_t retained_reference{};
  for(const auto& delta:grouped_raw_vertices){
    while(retained_raw_vertex!=retained_raw_end&&
          retained_raw_vertex->key<delta.vertex.key){
      next_intersections.push_back(*retained_raw_vertex++);
      next_intersection_references.push_back(
          cache->intersection_references[retained_reference++]);
    }
    const bool exists=retained_raw_vertex!=retained_raw_end&&
        retained_raw_vertex->key==delta.vertex.key;
    const auto old_references=exists?
        cache->intersection_references[retained_reference]:0U;
    if(delta.removed<0||delta.added<0||
       static_cast<std::uint32_t>(delta.removed)>old_references)
      throw std::logic_error("raw surface vertex reference underflow");
    const auto references=old_references-
        static_cast<std::uint32_t>(delta.removed)+
        static_cast<std::uint32_t>(delta.added);
    if(references>0U){
      next_intersections.push_back(
          delta.added>0?delta.vertex:*retained_raw_vertex);
      next_intersection_references.push_back(references);
    }
    if(exists){++retained_raw_vertex;++retained_reference;}
  }
  while(retained_raw_vertex!=retained_raw_end){
    next_intersections.push_back(*retained_raw_vertex++);
    next_intersection_references.push_back(
        cache->intersection_references[retained_reference++]);
  }
  const auto topology_built=std::chrono::steady_clock::now();
  const auto vertex_hash=[](const tetra::WorldVertexKey& key){
    std::uint64_t hash=static_cast<std::uint64_t>(key.x)*0x9e3779b97f4a7c15ULL;
    hash^=static_cast<std::uint64_t>(key.y)+(hash<<6U)+(hash>>2U);
    hash^=static_cast<std::uint64_t>(key.z)+(hash<<6U)+(hash>>2U);
    hash^=key.denominator_exponent+(hash<<6U)+(hash>>2U);
    return static_cast<std::size_t>(hash);
  };
  std::unordered_set<tetra::HierarchyBlockId,decltype(block_hash)>
      current_ids(0U,block_hash),changed_blocks(0U,block_hash),
      removed_blocks(0U,block_hash);
  current_ids.reserve(directory.hierarchy_blocks().size());
  changed_blocks.reserve(directory.hierarchy_blocks().size()/8U);
  for(const auto& signature:hierarchy){
    current_ids.insert(signature.id);
    if(hierarchy_payload_changed_blocks.contains(signature.id))
      changed_blocks.insert(signature.id);
  }
  if(field_changed)changed_blocks.insert(current_ids.begin(),current_ids.end());
  if(cache)for(const auto& previous:cache->hierarchy)
    if(!current_ids.contains(previous.id))removed_blocks.insert(previous.id);
  std::unordered_set<tetra::WorldVertexKey,decltype(vertex_hash)>
      changed_vertices(0U,vertex_hash);
  if(cache&&!field_changed)for(const auto& snapshot:cache->snapshots)
    // Seed from both sides of the topology change. A removed old surface edge
    // cannot be discovered by traversing only the replacement graph, but its
    // endpoint bases still identify every retained block whose optimized
    // Jacobi neighborhood depended on it.
    if(removed_blocks.contains(snapshot.id)||changed_blocks.contains(snapshot.id))
      for(const auto& vertex:snapshot.vertices)
        for(std::size_t basis=0;basis<vertex.key.basis_count;++basis)
          changed_vertices.insert(vertex.key.basis[basis]);

  std::vector<tetra::WorldDerivedVertexKey> optimizer_keys;
  std::vector<std::uint64_t> optimizer_incident_hashes;
  std::vector<std::uint32_t> optimizer_neighbor_offsets;
  std::vector<std::uint32_t> optimizer_neighbors;
  std::vector<std::uint32_t> optimizer_patch_distances;
  std::vector<tetra::WorldDerivedVertexKey> next_optimizer_stable_keys;
  std::vector<SparseWorldSurfaceCache::CountedOptimizerEdge>
      next_optimizer_edges,next_optimizer_reverse_edges;
  std::vector<std::uint32_t> optimizer_stable_to_current;
  std::vector<std::uint32_t> optimizer_current_to_stable;
  std::vector<std::uint8_t> optimizer_affected;
  std::set<tetra::WorldDerivedVertexKey> optimizer_affected_retired;
  const auto derived_vertex_hash=[](const tetra::WorldDerivedVertexKey& key){
    std::uint64_t hash=static_cast<std::uint8_t>(key.kind)+
        0x9e3779b97f4a7c15ULL;
    const auto add=[&](std::uint64_t value){
      hash^=value+0x9e3779b97f4a7c15ULL+(hash<<6U)+(hash>>2U);
    };
    add(key.basis_count);
    for(std::size_t basis=0;basis<key.basis_count;++basis){
      add(static_cast<std::uint64_t>(key.basis[basis].x));
      add(static_cast<std::uint64_t>(key.basis[basis].y));
      add(static_cast<std::uint64_t>(key.basis[basis].z));
      add(key.basis[basis].denominator_exponent);
    }
    return static_cast<std::size_t>(hash);
  };
  std::unordered_map<tetra::WorldDerivedVertexKey,std::uint32_t,
                     decltype(derived_vertex_hash)>
      optimizer_index(0U,derived_vertex_hash);
  if(cache&&optimize){
    optimizer_keys.reserve(next_intersections.size());
    for(const auto& vertex:next_intersections)
      optimizer_keys.push_back(vertex.key);
    optimizer_index.reserve(optimizer_keys.size());
    for(std::size_t index=0;index<optimizer_keys.size();++index){
      if(index>std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("optimizer dependency graph exceeds 32-bit indices");
      optimizer_index.emplace(
          optimizer_keys[index],static_cast<std::uint32_t>(index));
    }
    next_optimizer_stable_keys=cache->optimizer_stable_keys;
    std::unordered_map<tetra::WorldDerivedVertexKey,std::uint32_t,
                       decltype(derived_vertex_hash)>
        stable_index(0U,derived_vertex_hash);
    stable_index.reserve(next_optimizer_stable_keys.size()+optimizer_keys.size()/8U);
    for(std::size_t index=0;index<next_optimizer_stable_keys.size();++index)
      stable_index.emplace(next_optimizer_stable_keys[index],
                           static_cast<std::uint32_t>(index));
    const auto stable_id=[&](const tetra::WorldDerivedVertexKey& key){
      const auto found=stable_index.find(key);
      if(found!=stable_index.end())return found->second;
      if(next_optimizer_stable_keys.size()>
         std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("optimizer stable key directory exceeds 32-bit IDs");
      const auto id=static_cast<std::uint32_t>(next_optimizer_stable_keys.size());
      next_optimizer_stable_keys.push_back(key);stable_index.emplace(key,id);
      return id;
    };
    for(const auto& key:optimizer_keys)static_cast<void>(stable_id(key));
    struct EdgeDelta {
      std::array<std::uint32_t,2> vertices{};
      std::int32_t removed{},added{};
    };
    std::vector<EdgeDelta> edge_deltas;
    const bool edge_bootstrap=cache->optimizer_edges.empty();
    const auto append_triangle_edges=[&](
        const std::array<tetra::WorldDerivedVertexKey,3>& vertices,
        bool addition){
      std::array<std::uint32_t,3> ids{{stable_id(vertices[0]),
          stable_id(vertices[1]),stable_id(vertices[2])}};
      for(auto edge:std::array<std::array<std::uint32_t,2>,3>{{
          {{ids[0],ids[1]}},{{ids[1],ids[2]}},{{ids[2],ids[0]}}}}){
        if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
        edge_deltas.push_back({edge,addition?0:1,addition?1:0});
      }
    };
    if(!edge_bootstrap)for(const auto& snapshot:cache->snapshots)
      if(topology_changed_blocks.contains(snapshot.id)||
         !topology_current_blocks.contains(snapshot.id))
        for(const auto& triangle:snapshot.triangles)
          append_triangle_edges(triangle.vertices,false);
    for(const auto& block:current_raw_blocks)
      if(edge_bootstrap||topology_changed_blocks.contains(block->id))
        for(const auto& triangle:block->triangles){
          std::array<tetra::WorldDerivedVertexKey,3> keys;
          for(std::size_t corner=0;corner<3U;++corner)
            keys[corner]=block->vertices[triangle.vertices[corner]].key;
          append_triangle_edges(keys,true);
        }
    std::ranges::sort(edge_deltas,{},&EdgeDelta::vertices);
    std::vector<EdgeDelta> grouped_edges;
    for(const auto& delta:edge_deltas){
      if(grouped_edges.empty()||
         grouped_edges.back().vertices!=delta.vertices)
        grouped_edges.push_back(delta);
      else{
        grouped_edges.back().removed+=delta.removed;
        grouped_edges.back().added+=delta.added;
      }
    }
    const auto merge_edge_directory=[](
        const std::vector<SparseWorldSurfaceCache::CountedOptimizerEdge>& old,
        const std::vector<EdgeDelta>& deltas){
      std::vector<SparseWorldSurfaceCache::CountedOptimizerEdge> result;
      result.reserve(old.size()+deltas.size());
      auto retained=old.begin();
      for(const auto& delta:deltas){
        while(retained!=old.end()&&retained->vertices<delta.vertices)
          result.push_back(*retained++);
        const bool exists=retained!=old.end()&&
            retained->vertices==delta.vertices;
        const auto old_references=exists?retained->references:0U;
        if(delta.removed<0||delta.added<0||
           static_cast<std::uint32_t>(delta.removed)>old_references)
          throw std::logic_error("optimizer edge reference underflow");
        const auto references=old_references-
            static_cast<std::uint32_t>(delta.removed)+
            static_cast<std::uint32_t>(delta.added);
        if(references>0U)result.push_back({delta.vertices,references});
        if(exists)++retained;
      }
      result.insert(result.end(),retained,old.end());
      return result;
    };
    next_optimizer_edges=merge_edge_directory(cache->optimizer_edges,
                                               grouped_edges);
    auto reverse_deltas=grouped_edges;
    for(auto& delta:reverse_deltas)
      std::swap(delta.vertices[0],delta.vertices[1]);
    std::ranges::sort(reverse_deltas,{},&EdgeDelta::vertices);
    next_optimizer_reverse_edges=merge_edge_directory(
        cache->optimizer_reverse_edges,reverse_deltas);

    constexpr auto no_current=std::numeric_limits<std::uint32_t>::max();
    optimizer_stable_to_current.assign(next_optimizer_stable_keys.size(),
                                       no_current);
    optimizer_current_to_stable.resize(optimizer_keys.size());
    for(std::size_t current=0;current<optimizer_keys.size();++current){
      const auto stable=stable_index.find(optimizer_keys[current]);
      if(stable==stable_index.end())
        throw std::logic_error("optimizer vertex has no stable ID");
      optimizer_stable_to_current[stable->second]=
          static_cast<std::uint32_t>(current);
      optimizer_current_to_stable[current]=stable->second;
    }
    const auto visit_stable_neighbors=[&](std::uint32_t source,
                                          const auto& visit){
      const auto visit_directory=[&](const auto& directory){
        const std::array<std::uint32_t,2> lower{{source,0U}};
        auto edge=std::ranges::lower_bound(
            directory,lower,{},
            &SparseWorldSurfaceCache::CountedOptimizerEdge::vertices);
        while(edge!=directory.end()&&edge->vertices[0]==source){
          visit(edge->vertices[1]);++edge;
        }
      };
      visit_directory(next_optimizer_edges);
      visit_directory(next_optimizer_reverse_edges);
    };
    for(const auto& edge:next_optimizer_edges)
      if(edge.vertices[0]>=optimizer_stable_to_current.size()||
         edge.vertices[1]>=optimizer_stable_to_current.size()||
         optimizer_stable_to_current[edge.vertices[0]]==no_current||
         optimizer_stable_to_current[edge.vertices[1]]==no_current)
        throw std::logic_error("active optimizer edge has no active vertex");
    // Preserve the public retained-vector shape while adjacency itself lives
    // in the two stable flat edge directories above.
    optimizer_neighbor_offsets.assign(optimizer_keys.size()+1U,0U);
    optimizer_neighbors.clear();

    optimizer_incident_hashes.assign(optimizer_keys.size(),0U);
    optimizer_affected.assign(optimizer_keys.size(),0U);
    std::vector<std::uint32_t> frontier;
    const auto mark_affected=[&](const tetra::WorldDerivedVertexKey& key){
      const auto found=optimizer_index.find(key);
      if(found==optimizer_index.end()){
        optimizer_affected_retired.insert(key);return;
      }
      if(optimizer_affected[found->second]!=0U)
        return;
      optimizer_affected[found->second]=1U;frontier.push_back(found->second);
    };
    if(field_changed||edge_bootstrap){
      frontier.resize(optimizer_keys.size());
      std::iota(frontier.begin(),frontier.end(),0U);
      std::ranges::fill(optimizer_affected,std::uint8_t{1U});
      for(const auto& vertex:cache->intersections)mark_affected(vertex.key);
    }else{
      for(const auto& snapshot:cache->snapshots)
        if(topology_changed_blocks.contains(snapshot.id)||
           !topology_current_blocks.contains(snapshot.id))
          for(const auto& vertex:snapshot.vertices)mark_affected(vertex.key);
      for(const auto& triangle:all_surface_triangles)
        if(topology_changed_blocks.contains(tetra::hierarchy_block_id(
             triangle.owner,directory.block_generations())))
          for(const auto& key:triangle.vertices)mark_affected(key);
    }
    for(std::uint32_t ring=0;ring<surface_optimizer_passes&&!frontier.empty();++ring){
      std::vector<std::uint32_t> next;
      for(const auto current_vertex:frontier){
        const auto stable=optimizer_current_to_stable[current_vertex];
        visit_stable_neighbors(stable,[&](std::uint32_t neighbor){
        const auto current=optimizer_stable_to_current[neighbor];
        if(current!=no_current&&optimizer_affected[current]==0U){
          optimizer_affected[current]=1U;next.push_back(current);
        }
        });
      }
      frontier=std::move(next);
    }
  }
  const auto optimizer_is_affected=[&](
      const tetra::WorldDerivedVertexKey& key){
    const auto found=optimizer_index.find(key);
    return (found!=optimizer_index.end()&&optimizer_affected[found->second]!=0U)||
        optimizer_affected_retired.contains(key);
  };
  const auto optimizer_dependencies_built=std::chrono::steady_clock::now();
  std::map<tetra::HierarchyBlockId,
           std::vector<tetra::WorldDerivedVertexKey>> block_surface_vertices;
  if(cache){
    for(const auto& block:current_raw_blocks){
      auto& surface_vertices=block_surface_vertices[block->id];
      surface_vertices.reserve(block->vertices.size());
      for(const auto& vertex:block->vertices)
        surface_vertices.push_back(vertex.key);
    }
  }else for(const auto& block:volume.blocks){
    auto& surface_vertices=block_surface_vertices[block->id];
    for(const auto& cell:block->cells){
      std::array<double,4> distances{};
      for(std::size_t corner=0;corner<4U;++corner)
        distances[corner]=field.signed_distance(
            domain.to_world(cell.positions[corner]));
      for(const auto edge:surface_edges)
        if((distances[edge[0]]<0.0)!=(distances[edge[1]]<0.0))
          surface_vertices.push_back(tetra::world_edge_intersection_key(
              cell.vertices[edge[0]],cell.vertices[edge[1]]));
    }
  }
  for(auto& [id,surface_vertices]:block_surface_vertices){
    (void)id;
    std::ranges::sort(surface_vertices);
    surface_vertices.erase(
        std::unique(surface_vertices.begin(),surface_vertices.end()),
        surface_vertices.end());
  }
  const auto expand_shared_blocks=[&](auto& blocks,unsigned int rings){
    for(unsigned int ring=0;ring<rings;++ring){
      std::set<tetra::WorldDerivedVertexKey> shared;
      for(const auto& [id,vertices]:block_surface_vertices)
        if(blocks.contains(id))shared.insert(vertices.begin(),vertices.end());
      const auto before=blocks.size();
      for(const auto& [id,vertices]:block_surface_vertices)
        if(!blocks.contains(id)&&std::ranges::any_of(
             vertices,[&](const auto& key){return shared.contains(key);}))
          blocks.insert(id);
      if(blocks.size()==before)break;
    }
  };
  std::unordered_set<tetra::HierarchyBlockId,decltype(block_hash)>
      output_blocks(0U,block_hash);
  if(cache&&optimize){
    for(const auto& [id,keys]:block_surface_vertices)
      if(std::ranges::any_of(keys,[&](const auto& key){
           return optimizer_is_affected(key);
         }))output_blocks.insert(id);
    for(const auto& snapshot:cache->snapshots)
      if(current_ids.contains(snapshot.id)&&std::ranges::any_of(
          snapshot.vertices,[&](const auto& vertex){
            return optimizer_is_affected(vertex.key);
          }))output_blocks.insert(snapshot.id);
    // A triangle can retain the same geometric one-ring while changing its
    // canonical owner block. Include changed hierarchy payloads for that
    // ownership-only case; unchanged interior-only blocks add no triangles.
    for(const auto id:changed_blocks)
      if(block_surface_vertices.contains(id))output_blocks.insert(id);
    changed_blocks.clear();
    constexpr auto unreachable=std::numeric_limits<std::uint32_t>::max();
    optimizer_patch_distances.assign(optimizer_keys.size(),unreachable);
    std::deque<std::uint32_t> frontier;
    for(const auto& [id,keys]:block_surface_vertices){
      if(!output_blocks.contains(id))continue;
      for(const auto& key:keys){
        const auto found=optimizer_index.find(key);
        if(found==optimizer_index.end())
          throw std::logic_error("surface output block has no optimizer vertex");
        if(optimizer_patch_distances[found->second]==0U)continue;
        optimizer_patch_distances[found->second]=0U;
        frontier.push_back(found->second);
      }
    }
    while(!frontier.empty()){
      const auto vertex=frontier.front();frontier.pop_front();
      const auto distance=optimizer_patch_distances[vertex];
      if(distance>=surface_optimizer_dependency_halo_rings)continue;
      const auto visit_directory=[&](const auto& directory){
        const auto stable=optimizer_current_to_stable[vertex];
        const std::array<std::uint32_t,2> lower{{stable,0U}};
        auto edge=std::ranges::lower_bound(
            directory,lower,{},
            &SparseWorldSurfaceCache::CountedOptimizerEdge::vertices);
        while(edge!=directory.end()&&edge->vertices[0]==stable){
          const auto neighbor=optimizer_stable_to_current[edge->vertices[1]];
          if(neighbor==unreachable){++edge;continue;}
          if(optimizer_patch_distances[neighbor]>distance+1U){
            optimizer_patch_distances[neighbor]=distance+1U;
            frontier.push_back(neighbor);
          }
          ++edge;
        }
      };
      visit_directory(next_optimizer_edges);
      visit_directory(next_optimizer_reverse_edges);
    }
  }else if(cache){
    if(!changed_vertices.empty())
      for(const auto& [id,vertices]:block_surface_vertices)
        if(std::ranges::any_of(vertices,[&](const auto& key){
             for(std::size_t basis=0;basis<key.basis_count;++basis)
               if(changed_vertices.contains(key.basis[basis]))return true;
             return false;
           }))changed_blocks.insert(id);
    expand_shared_blocks(changed_blocks,1U);
    output_blocks.insert(changed_blocks.begin(),changed_blocks.end());
  }else{
    changed_blocks.insert(current_ids.begin(),current_ids.end());
    output_blocks.insert(current_ids.begin(),current_ids.end());
  }
  std::vector<tetra::WorldDerivedSurfaceSnapshot> reused_snapshots;
  if(cache)for(const auto& snapshot:cache->snapshots)
    if(current_ids.contains(snapshot.id)&&!output_blocks.contains(snapshot.id))
      reused_snapshots.push_back(snapshot);
  std::map<tetra::WorldDerivedVertexKey,tetra::Vec3> vertices;
  std::vector<KeyTriangle> triangles;
  if(cache){
    for(const auto& block:current_raw_blocks){
      if(!optimize&&!changed_blocks.contains(block->id))continue;
      for(const auto& compact:block->triangles){
        KeyTriangle triangle;triangle.owner=compact.owner;
        for(std::size_t corner=0;corner<3U;++corner)
          triangle.vertices[corner]=
              block->vertices[compact.vertices[corner]].key;
        if(optimize){
          const bool in_patch=std::ranges::any_of(
              triangle.vertices,[&](const auto& key){
                const auto found=optimizer_index.find(key);
                return found!=optimizer_index.end()&&
                    optimizer_patch_distances[found->second]<=
                        surface_optimizer_dependency_halo_rings-1U;
              });
          if(!in_patch)continue;
        }
        triangles.push_back(triangle);
      }
    }
  }else{
  std::size_t materialized_cell_count{};
  for(const auto& block:volume.blocks)
    materialized_cell_count+=block->cells.size();
  triangles.reserve(materialized_cell_count*2U);
  std::size_t cell_index{};
  for(const auto& volume_block:volume.blocks){
    if(cache&&!changed_blocks.contains(volume_block->id)){
      cell_index+=volume_block->cells.size();continue;
    }
    for(const auto& cell:volume_block->cells){
    if((cell_index++&255U)==0U&&cancellation.stop_requested())
      throw std::runtime_error("sparse world surface build canceled");
    std::array<tetra::Vec3,4> points{};
    std::array<double,4> distances{};
    for(std::size_t corner=0;corner<4U;++corner){
      points[corner]=domain.to_world(cell.positions[corner]);
      distances[corner]=field.signed_distance(points[corner]);
    }
    struct Crossing { tetra::WorldDerivedVertexKey key{};tetra::Vec3 point{}; };
    std::array<Crossing,4> crossings{};
    std::size_t crossing_count{};
    for(const auto edge:surface_edges){
      if((distances[edge[0]]<0.0)==(distances[edge[1]]<0.0))continue;
      const auto key=tetra::world_edge_intersection_key(
          cell.vertices[edge[0]],cell.vertices[edge[1]]);
      tetra::Vec3 point;
      const auto cached=cache?std::ranges::lower_bound(
          cache->intersections,key,{},&tetra::WorldSurfaceVertex::key):
          std::vector<tetra::WorldSurfaceVertex>::const_iterator{};
      if(cache&&cached!=cache->intersections.end()&&cached->key==key){
        point=cached->position;++reused_intersections;
      }else{
        const bool ordered=cell.vertices[edge[0]]<cell.vertices[edge[1]];
        point=field.edge_intersection(
            points[ordered?edge[0]:edge[1]],points[ordered?edge[1]:edge[0]]);
        ++computed_intersections;
      }
      crossings[crossing_count++]={key,point};
      const auto [found,inserted]=vertices.emplace(key,point);
      if(!inserted){
        const auto delta=found->second-point;
        if(dot(delta,delta)>1.0e-20)
          throw std::logic_error(
              "sparse world cells disagree on a shared field crossing");
      }
    }
    if(crossing_count<3U)continue;
    tetra::Vec3 centre{};
    for(std::size_t index=0;index<crossing_count;++index)
      centre=centre+crossings[index].point;
    centre=centre/static_cast<double>(crossing_count);
    const auto normal=field.normal(centre);
    const auto reference=std::abs(normal.z)<0.9?tetra::Vec3{0.0,0.0,1.0}:
        tetra::Vec3{0.0,1.0,0.0};
    const auto axis_u=cross(reference,normal),axis_v=cross(normal,axis_u);
    std::sort(crossings.begin(),
        crossings.begin()+static_cast<std::ptrdiff_t>(crossing_count),
        [&](const Crossing& first,const Crossing& second){
          const auto a=first.point-centre,b=second.point-centre;
          return std::atan2(dot(a,axis_v),dot(a,axis_u))<
                 std::atan2(dot(b,axis_v),dot(b,axis_u));
        });
    for(std::size_t index=1U;index+1U<crossing_count;++index)
      triangles.push_back({{{crossings[0].key,crossings[index].key,
                             crossings[index+1U].key}},cell.logical_owner});
    }
  }
  }
  const auto extracted=std::chrono::steady_clock::now();

  OptimizedSurface surface;
  if(cache){
    surface.global_keys.reserve(triangles.size()*3U);
    for(const auto& triangle:triangles)
      surface.global_keys.insert(surface.global_keys.end(),
                                 triangle.vertices.begin(),
                                 triangle.vertices.end());
    std::ranges::sort(surface.global_keys);
    surface.global_keys.erase(
        std::unique(surface.global_keys.begin(),surface.global_keys.end()),
        surface.global_keys.end());
    surface.positions.reserve(surface.global_keys.size());
    for(const auto& key:surface.global_keys){
      const auto found=std::ranges::lower_bound(
          next_intersections,key,{},&tetra::WorldSurfaceVertex::key);
      if(found==next_intersections.end()||found->key!=key)
        throw std::logic_error(
            "direct sparse surface triangle has no crossing");
      surface.positions.push_back(found->position);
    }
  }else{
    surface.positions.reserve(vertices.size());
    surface.global_keys.reserve(vertices.size());
    for(const auto& [key,position]:vertices){
      surface.global_keys.push_back(key);surface.positions.push_back(position);
    }
  }
  std::unordered_map<tetra::WorldDerivedVertexKey,std::size_t,
                     decltype(derived_vertex_hash)>
      patch_index(0U,derived_vertex_hash);
  patch_index.reserve(surface.global_keys.size());
  for(std::size_t index=0;index<surface.global_keys.size();++index)
    patch_index.emplace(surface.global_keys[index],index);
  surface.triangles.reserve(triangles.size());
  for(const auto& triangle:triangles){
    std::array<std::size_t,3> indices{};
    for(std::size_t corner=0;corner<3U;++corner){
      const auto found=patch_index.find(triangle.vertices[corner]);
      if(found==patch_index.end())
        throw std::logic_error("sparse world triangle has no vertex");
      indices[corner]=found->second;
    }
    surface.triangles.push_back(indices);
  }
  std::vector<std::uint32_t> patch_dependency_distances;
  if(cache&&optimize){
    patch_dependency_distances.reserve(surface.global_keys.size());
    for(const auto& key:surface.global_keys){
      const auto found=optimizer_index.find(key);
      if(found==optimizer_index.end())
        throw std::logic_error("optimizer patch has no global dependency vertex");
      patch_dependency_distances.push_back(
          optimizer_patch_distances[found->second]);
    }
  }
  PreparedScene optimization_metrics;
  if(optimize&&!surface.triangles.empty())
    optimize_surface_graph(
        optimization_metrics,field,surface,patch_dependency_distances,{},executor);
  const auto optimized=std::chrono::steady_clock::now();

  struct BlockOutput {
    tetra::WorldDerivedSurfaceSnapshot snapshot;
    std::vector<tetra::WorldDerivedVertexKey> vertices;
  };
  std::map<tetra::HierarchyBlockId,BlockOutput> blocks;
  for(std::size_t index=0;index<triangles.size();++index){
    const auto block_id=tetra::hierarchy_block_id(
        triangles[index].owner,directory.block_generations());
    if(cache&&!output_blocks.contains(block_id))continue;
    auto& output=blocks[block_id];
    output.snapshot.id=block_id;
    output.snapshot.source_hierarchy_revision=directory.revision();
    output.snapshot.metrics.optimizer_passes=optimize?surface_optimizer_passes:0U;
    output.snapshot.metrics.dependency_halo_rings=
        optimize?surface_optimizer_dependency_halo_rings:0U;
    output.snapshot.triangles.push_back(
        {triangles[index].vertices,triangles[index].owner});
    output.vertices.insert(output.vertices.end(),
                           triangles[index].vertices.begin(),
                           triangles[index].vertices.end());
  }
  std::vector<tetra::WorldDerivedSurfaceSnapshot> snapshots;
  snapshots=std::move(reused_snapshots);
  snapshots.reserve(snapshots.size()+blocks.size());
  for(auto& [id,output]:blocks){
    std::ranges::sort(output.vertices);
    output.vertices.erase(
        std::unique(output.vertices.begin(),output.vertices.end()),
        output.vertices.end());
    for(const auto& key:output.vertices){
      const auto found=patch_index.find(key);
      if(found==patch_index.end())
        throw std::logic_error("surface snapshot has no optimized vertex");
      const auto index=found->second;
      output.snapshot.vertices.push_back({key,surface.positions[index]});
    }
    // The native world path rebuilds the complete five-pass optimization
    // patch selected above. Its immutable snapshot therefore depends on its
    // owning hierarchy block; stage_derived_surfaces records that self
    // dependency without duplicating thousands of global block identifiers
    // into every surface payload.
    output.snapshot.metrics.vertices=output.snapshot.vertices.size();
    output.snapshot.metrics.triangles=output.snapshot.triangles.size();
    output.snapshot.metrics.dependency_blocks=
        output.snapshot.dependency_blocks.size();
    snapshots.push_back(std::move(output.snapshot));
  }
  std::ranges::sort(snapshots,{},&tetra::WorldDerivedSurfaceSnapshot::id);
  for(auto& snapshot:snapshots)
    snapshot.source_hierarchy_revision=directory.revision();
  std::vector<SparseWorldSurfaceCache::CountedSurfaceVertex>
      next_assembled_vertices;
  std::vector<SparseWorldSurfaceCache::CountedSurfaceTriangle>
      next_assembled_triangles;
  auto result=cache?assemble_blocked_snapshots_incremental(
      std::move(snapshots),*cache,output_blocks,next_assembled_vertices,
      next_assembled_triangles,assemble_flat_output,
      optimize?std::span(next_optimizer_stable_keys):
               std::span<const tetra::WorldDerivedVertexKey>{},
      optimize?std::span(optimizer_current_to_stable):
               std::span<const std::uint32_t>{},
      optimize?&optimizer_index:nullptr):
      assemble_blocked_snapshots(std::move(snapshots));
  result.metrics.block_generations=directory.block_generations();
  result.metrics.source_vertices=cache?next_assembled_vertices.size():
      result.vertices.size();
  result.metrics.source_triangles=cache?next_assembled_triangles.size():
      result.triangles.size();
  result.metrics.conforming_cells=volume.cells;
  result.metrics.transition_cells=volume.transition_cells;
  result.metrics.conforming_volume_hash=conforming_volume_hash;
  result.metrics.reused_intersections=reused_intersections;
  result.metrics.computed_intersections=computed_intersections;
  result.metrics.reused_surface_blocks=cache?result.snapshots.size()-blocks.size():0U;
  result.metrics.rebuilt_surface_blocks=blocks.size();
  result.metrics.reused_conforming_cells=volume.reused_cells;
  result.metrics.rebuilt_conforming_cells=volume.rebuilt_cells;
  result.metrics.conforming_owners_considered=volume.owners_considered;
  result.metrics.green_cells_enumerated=
      volume.green_cells_enumerated+direct_green_cells_enumerated;
  result.metrics.conforming_cells_materialized=volume.materialized_cells;
  result.metrics.surface_candidate_owners=surface_candidate_owners;
  result.metrics.surface_candidate_blocks=surface_candidate_blocks.size();
  result.metrics.surface_classification_samples=surface_classification_samples;
  result.metrics.reused_surface_certificates=reused_surface_certificates;
  result.metrics.rebuilt_surface_certificates=rebuilt_surface_certificates;
  result.metrics.optimizer_dependency_vertices=optimizer_keys.size();
  result.metrics.affected_optimizer_vertices=static_cast<std::size_t>(
      std::ranges::count(optimizer_affected,std::uint8_t{1U}));
  result.metrics.retained_optimizer_dependency_bytes=
      optimizer_incident_hashes.capacity()*sizeof(std::uint64_t)+
      optimizer_neighbor_offsets.capacity()*sizeof(std::uint32_t)+
      optimizer_neighbors.capacity()*sizeof(std::uint32_t)+
      next_optimizer_stable_keys.capacity()*
          sizeof(tetra::WorldDerivedVertexKey)+
      next_optimizer_edges.capacity()*
          sizeof(SparseWorldSurfaceCache::CountedOptimizerEdge)+
      next_optimizer_reverse_edges.capacity()*
          sizeof(SparseWorldSurfaceCache::CountedOptimizerEdge);
  result.metrics.surface_blocks=result.snapshots.size();
  result.metrics.total_core_vertices=result.metrics.source_vertices;
  result.metrics.total_patch_vertices=result.metrics.source_vertices;
  result.metrics.total_patch_triangles=result.metrics.source_triangles;
  const auto snapshots_assembled=std::chrono::steady_clock::now();
  if(cache){
    if(optimize){
      // Retain raw edge crossings, not their optimized surface positions.
      // A derived key names the source hierarchy edge and must remain valid
      // when a neighbouring output block is rebuilt later.
      cache->intersections=std::move(next_intersections);
      cache->intersection_references=std::move(next_intersection_references);
      if(cache->intersections.size()!=optimizer_keys.size()||
         !std::equal(cache->intersections.begin(),cache->intersections.end(),
             optimizer_keys.begin(),[](const auto& vertex,const auto& key){
               return vertex.key==key;
             }))
        throw std::logic_error("optimizer dependency keys do not match surface");
      cache->optimizer_incident_hashes=std::move(optimizer_incident_hashes);
      cache->optimizer_neighbor_offsets=std::move(optimizer_neighbor_offsets);
      cache->optimizer_neighbors=std::move(optimizer_neighbors);
      cache->optimizer_stable_keys=std::move(next_optimizer_stable_keys);
      cache->optimizer_edges=std::move(next_optimizer_edges);
      cache->optimizer_reverse_edges=std::move(next_optimizer_reverse_edges);
    }else{
      cache->intersections=std::move(next_intersections);
      cache->intersection_references=std::move(next_intersection_references);
      cache->optimizer_incident_hashes.clear();
      cache->optimizer_neighbor_offsets.clear();
      cache->optimizer_neighbors.clear();
      cache->optimizer_stable_keys.clear();
      cache->optimizer_edges.clear();
      cache->optimizer_reverse_edges.clear();
    }
    cache->hierarchy=std::move(hierarchy);
    cache->surface_certificate_blocks=std::move(certificate_blocks);
    cache->surface_field_signature=field_signature;
    cache->surface_source_hierarchy_revision=directory.revision();
    cache->assembled_vertices=std::move(next_assembled_vertices);
    cache->assembled_triangles=std::move(next_assembled_triangles);
    cache->assembled_triangles_use_optimizer_stable_ids=optimize;
    if(!restrict_retained_volume)cache->conforming=std::move(volume);
    else{
      tetra::WorldBlockedConformingVolume retained;
      for(const auto id:retained_volume_blocks){
        const auto found=std::ranges::find_if(
            volume.blocks,[&](const auto& block){return block->id==id;});
        if(found==volume.blocks.end()||(*found)->id!=id)
          throw std::logic_error("world volume pin names no conforming block");
        const auto& block=**found;
        retained.blocks.push_back(*found);
        retained.cells+=block.cells.size();
        retained.transition_cells+=block.transition_cells;
        retained.logical_owners+=block.logical_owners;
        retained.retained_bytes+=sizeof(tetra::WorldConformingBlockSnapshot)+
            block.green_masks.capacity()*sizeof(std::uint8_t)+
            block.cells.capacity()*sizeof(tetra::WorldConformingCell);
        const auto previous=std::ranges::lower_bound(
            previous_conforming_blocks,id,{},
            [](const auto& candidate){return candidate->id;});
        if(previous!=previous_conforming_blocks.end()&&(*previous)->id==id&&
           previous->get()==found->get()){
          ++retained.reused_blocks;
          retained.reused_cells+=block.cells.size();
        }else{
          ++retained.rebuilt_blocks;
          retained.rebuilt_cells+=block.cells.size();
        }
      }
      if(retained.cells!=volume.materialized_cells)
        throw std::logic_error("retained conforming cell accounting mismatch: "+
            std::to_string(retained.cells)+" != "+
            std::to_string(volume.materialized_cells)+" cells across "+
            std::to_string(retained.blocks.size())+" requested / "+
            std::to_string(volume.blocks.size())+" materialized blocks ("+
            std::to_string(retained_volume_blocks.size())+" pin ids)");
      cache->conforming=std::move(retained);
    }
    cache->raw_blocks=std::move(current_raw_blocks);
    cache->snapshots=result.snapshots;
  }
  const auto assembled=std::chrono::steady_clock::now();
  result.metrics.classification_milliseconds=
      std::chrono::duration<double,std::milli>(classified-started).count();
  result.metrics.conforming_materialization_milliseconds=
      std::chrono::duration<double,std::milli>(
          volume_reconstructed-classified).count();
  result.metrics.topology_milliseconds=
      std::chrono::duration<double,std::milli>(
          topology_built-volume_reconstructed).count();
  result.metrics.optimizer_dependency_milliseconds=
      std::chrono::duration<double,std::milli>(
          optimizer_dependencies_built-topology_built).count();
  result.metrics.patch_extraction_milliseconds=
      std::chrono::duration<double,std::milli>(
          extracted-optimizer_dependencies_built).count();
  result.metrics.volume_reconstruction_milliseconds=
      std::chrono::duration<double,std::milli>(volume_reconstructed-started).count();
  result.metrics.extraction_milliseconds=
      std::chrono::duration<double,std::milli>(extracted-volume_reconstructed).count();
  result.metrics.optimization_milliseconds=
      std::chrono::duration<double,std::milli>(optimized-extracted).count();
  result.metrics.snapshot_assembly_milliseconds=
      std::chrono::duration<double,std::milli>(
          snapshots_assembled-optimized).count();
  result.metrics.cache_publication_milliseconds=
      std::chrono::duration<double,std::milli>(
          assembled-snapshots_assembled).count();
  result.metrics.assembly_milliseconds=
      std::chrono::duration<double,std::milli>(assembled-optimized).count();
  result.metrics.build_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  return result;
}

BlockedDerivedSurfaceBuild build_blocked_derived_surface(
    const tetra::TetMesh& mesh,const tetra::WorldCutDirectory& directory,
    const tetra::Sphere& sphere,BlockedDerivedSurfaceOptions options,
    tetra::GeometryExecutor* executor,std::stop_token cancellation) {
  const auto started=std::chrono::steady_clock::now();
  if(options.operation_budget==0U)
    throw std::invalid_argument("blocked surface operation budget must be nonzero");
  if(options.job_group_size==0U)
    throw std::invalid_argument("blocked surface job group size must be nonzero");
  if(mesh.subdivision_method()!=tetra::SubdivisionMethod::bcc_red_green)
    throw std::invalid_argument("blocked surfaces require BCC red-green hierarchy");
  const auto check_canceled=[&]{
    if(cancellation.stop_requested())
      throw std::runtime_error("blocked surface build canceled");
  };
  check_canceled();

  std::vector<tetra::WorldTetAddress> mesh_owners;
  mesh_owners.reserve(mesh.logical_red_owners().size());
  for(const auto owner:mesh.logical_red_owners())
    mesh_owners.push_back(tetra::world_tet_address(owner));
  std::ranges::sort(mesh_owners);
  std::vector<tetra::WorldTetAddress> directory_owners;
  directory_owners.reserve(directory.logical_owner_count());
  directory.for_each_logical_owner(
      [&](tetra::WorldTetAddress owner){directory_owners.push_back(owner);});
  std::ranges::sort(directory_owners);
  if(mesh_owners!=directory_owners)
    throw std::invalid_argument(
        "blocked surface directory does not match the TetMesh logical cut");

  PreparedScene topology;
  build_adaptive_cleaved_volume(topology,mesh,sphere,
      VolumeConnectionMethod::adaptive_cleaving,StencilConstruction::fixed,
      StencilSelectionObjective::balanced);
  auto boundary=connected_surface_boundary_faces(topology);
  for(auto& face:boundary){
    const auto a=topology.connected_volume_vertices[face.vertices[0]];
    const auto b=topology.connected_volume_vertices[face.vertices[1]];
    const auto c=topology.connected_volume_vertices[face.vertices[2]];
    const auto normal=face_normal(a,b,c),centre=(a+b+c)/3.0;
    const auto outward=sphere.normal(centre);
    if(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z<0.0)
      std::swap(face.vertices[1],face.vertices[2]);
  }
  check_canceled();

  std::map<tetra::TetId,tetra::WorldTetAddress> logical_owner_by_cell;
  const auto volume=mesh.conforming_volume();
  for(std::size_t index=0;index<volume.size();++index){
    const auto cell=volume.cell(index);
    logical_owner_by_cell.emplace(
        cell.address,tetra::world_tet_address(cell.logical_owner));
  }
  std::vector<tetra::WorldTetAddress> triangle_owners;
  std::vector<tetra::HierarchyBlockId> triangle_blocks;
  triangle_owners.reserve(boundary.size());triangle_blocks.reserve(boundary.size());
  for(const auto& face:boundary){
    const auto parent=topology.connected_volume_parents.at(face.tetrahedron);
    const auto owner=logical_owner_by_cell.find(parent);
    if(owner==logical_owner_by_cell.end())
      throw std::logic_error("blocked surface boundary lacks a logical owner");
    const auto lookup=directory.lookup(owner->second);
    if(!lookup)throw std::logic_error("blocked surface owner is not resident");
    triangle_owners.push_back(owner->second);
    triangle_blocks.push_back(lookup.block->id);
  }

  std::vector<std::array<std::size_t,2>> edges;
  edges.reserve(boundary.size()*3U);
  for(const auto& face:boundary)
    for(auto edge:std::array<std::array<std::size_t,2>,3>{{
        {{face.vertices[0],face.vertices[1]}},
        {{face.vertices[1],face.vertices[2]}},
        {{face.vertices[2],face.vertices[0]}}}}){
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      edges.push_back(edge);
    }
  std::ranges::sort(edges);
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  const auto vertex_count=topology.connected_volume_vertices.size();
  std::vector<std::size_t> neighbor_offsets(vertex_count+1U);
  std::vector<std::size_t> incident_face_offsets(vertex_count+1U);
  std::vector<std::size_t> incident_tet_offsets(vertex_count+1U);
  for(const auto edge:edges){
    ++neighbor_offsets[edge[0]+1U];++neighbor_offsets[edge[1]+1U];
  }
  for(const auto& face:boundary)
    for(const auto vertex:face.vertices)++incident_face_offsets[vertex+1U];
  for(const auto& tet:topology.connected_volume_tetrahedra)
    for(const auto vertex:tet)++incident_tet_offsets[vertex+1U];
  for(std::size_t vertex=1;vertex<neighbor_offsets.size();++vertex){
    neighbor_offsets[vertex]+=neighbor_offsets[vertex-1U];
    incident_face_offsets[vertex]+=incident_face_offsets[vertex-1U];
    incident_tet_offsets[vertex]+=incident_tet_offsets[vertex-1U];
  }
  std::vector<std::size_t> neighbors(neighbor_offsets.back());
  std::vector<std::size_t> incident_faces(incident_face_offsets.back());
  std::vector<std::size_t> incident_tets(incident_tet_offsets.back());
  auto neighbor_cursor=neighbor_offsets;
  auto face_cursor=incident_face_offsets,tet_cursor=incident_tet_offsets;
  for(const auto edge:edges){
    neighbors[neighbor_cursor[edge[0]]++]=edge[1];
    neighbors[neighbor_cursor[edge[1]]++]=edge[0];
  }
  for(std::size_t face=0;face<boundary.size();++face)
    for(const auto vertex:boundary[face].vertices)
      incident_faces[face_cursor[vertex]++]=face;
  for(std::size_t tet=0;tet<topology.connected_volume_tetrahedra.size();++tet)
    for(const auto vertex:topology.connected_volume_tetrahedra[tet])
      incident_tets[tet_cursor[vertex]++]=tet;
  for(std::size_t vertex=0;vertex<vertex_count;++vertex)
    std::sort(neighbors.begin()+static_cast<std::ptrdiff_t>(neighbor_offsets[vertex]),
              neighbors.begin()+static_cast<std::ptrdiff_t>(neighbor_offsets[vertex+1U]),
              [&](std::size_t first,std::size_t second){
                return topology.connected_volume_global_keys[first]<
                       topology.connected_volume_global_keys[second];
              });

  struct BlockJob {
    tetra::HierarchyBlockId id{};
    std::vector<std::size_t> triangles;
  };
  std::map<tetra::HierarchyBlockId,std::vector<std::size_t>> owned;
  for(std::size_t triangle=0;triangle<triangle_blocks.size();++triangle)
    owned[triangle_blocks[triangle]].push_back(triangle);
  std::vector<BlockJob> jobs;
  jobs.reserve(owned.size());
  for(auto& [id,triangles]:owned)jobs.push_back({id,std::move(triangles)});
  if(options.reverse_job_order)std::ranges::reverse(jobs);

  struct JobResult {
    tetra::WorldDerivedSurfaceSnapshot snapshot;
    std::size_t core_vertices{};
    std::size_t patch_vertices{};
    std::size_t patch_triangles{};
    double minimum_connected_tet_quality_after{1.0};
    bool connected_volume_valid{true};
  };
  std::vector<JobResult> completed(jobs.size());
  const auto build_job=[&](std::size_t job_index){
    check_canceled();
    const auto& job=jobs[job_index];
    constexpr auto unreachable=std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> distance(vertex_count,unreachable);
    std::deque<std::size_t> frontier;
    for(const auto triangle:job.triangles)
      for(const auto vertex:boundary[triangle].vertices)
        if(distance[vertex]!=0U){distance[vertex]=0U;frontier.push_back(vertex);}
    while(!frontier.empty()){
      const auto vertex=frontier.front();frontier.pop_front();
      if(distance[vertex]>=surface_optimizer_dependency_halo_rings)continue;
      for(std::size_t offset=neighbor_offsets[vertex];
          offset<neighbor_offsets[vertex+1U];++offset){
        const auto neighbor=neighbors[offset];
        if(distance[neighbor]<=distance[vertex]+1U)continue;
        distance[neighbor]=distance[vertex]+1U;frontier.push_back(neighbor);
      }
    }

    std::vector<std::size_t> patch_faces;
    for(std::size_t face=0;face<boundary.size();++face)
      if(std::ranges::any_of(boundary[face].vertices,[&](std::size_t vertex){
           return distance[vertex]<surface_optimizer_dependency_halo_rings;
         }))
        patch_faces.push_back(face);
    std::vector<std::size_t> patch_tets;
    std::vector<std::uint8_t> included_tet(
        topology.connected_volume_tetrahedra.size(),0U);
    for(std::size_t vertex=0;vertex<vertex_count;++vertex){
      if(distance[vertex]>=surface_optimizer_dependency_halo_rings)continue;
      for(std::size_t offset=incident_tet_offsets[vertex];
          offset<incident_tet_offsets[vertex+1U];++offset){
        const auto tet=incident_tets[offset];
        if(included_tet[tet]==0U){included_tet[tet]=1U;patch_tets.push_back(tet);}
      }
    }
    std::ranges::sort(patch_tets,[&](std::size_t first,std::size_t second){
      const auto first_parent=topology.connected_volume_parents[first];
      const auto second_parent=topology.connected_volume_parents[second];
      if(first_parent!=second_parent)return first_parent<second_parent;
      std::array<tetra::WorldDerivedVertexKey,4> first_key{},second_key{};
      for(std::size_t corner=0;corner<4U;++corner){
        first_key[corner]=topology.connected_volume_global_keys[
            topology.connected_volume_tetrahedra[first][corner]];
        second_key[corner]=topology.connected_volume_global_keys[
            topology.connected_volume_tetrahedra[second][corner]];
      }
      std::ranges::sort(first_key);std::ranges::sort(second_key);
      return first_key<second_key;
    });
    std::vector<std::size_t> patch_vertices;
    patch_vertices.reserve(patch_tets.size()*2U);
    for(const auto tet:patch_tets)
      for(const auto vertex:topology.connected_volume_tetrahedra[tet])
        patch_vertices.push_back(vertex);
    std::ranges::sort(patch_vertices,[&](std::size_t first,std::size_t second){
      return topology.connected_volume_global_keys[first]<
             topology.connected_volume_global_keys[second];
    });
    patch_vertices.erase(std::unique(patch_vertices.begin(),patch_vertices.end()),
                         patch_vertices.end());
    std::vector<std::size_t> local_index(vertex_count,
        std::numeric_limits<std::size_t>::max());
    PreparedScene patch;
    patch.connected_volume_vertices.reserve(patch_vertices.size());
    patch.connected_volume_vertex_kinds.reserve(patch_vertices.size());
    patch.connected_volume_global_keys.reserve(patch_vertices.size());
    patch.connected_volume_source_edges.reserve(patch_vertices.size());
    patch.connected_volume_surface_vertices.reserve(patch_vertices.size());
    std::vector<std::uint32_t> patch_distances;
    patch_distances.reserve(patch_vertices.size());
    for(std::size_t local=0;local<patch_vertices.size();++local){
      const auto global=patch_vertices[local];local_index[global]=local;
      patch.connected_volume_vertices.push_back(
          topology.connected_volume_vertices[global]);
      patch.connected_volume_vertex_kinds.push_back(
          topology.connected_volume_vertex_kinds[global]);
      patch.connected_volume_global_keys.push_back(
          topology.connected_volume_global_keys[global]);
      patch.connected_volume_source_edges.push_back(
          topology.connected_volume_source_edges[global]);
      const auto surface=topology.connected_volume_surface_vertices[global];
      patch.connected_volume_surface_vertices.push_back(surface);
      patch_distances.push_back(surface!=0U?distance[global]:unreachable);
    }
    patch.connected_volume_tetrahedra.reserve(patch_tets.size());
    patch.connected_volume_parents.reserve(patch_tets.size());
    patch.connected_volume_boundary.reserve(patch_tets.size());
    patch.connected_volume_regions.reserve(patch_tets.size());
    for(const auto tet:patch_tets){
      const auto& global=topology.connected_volume_tetrahedra[tet];
      patch.connected_volume_tetrahedra.push_back({{
          local_index[global[0]],local_index[global[1]],
          local_index[global[2]],local_index[global[3]]}});
      patch.connected_volume_parents.push_back(
          topology.connected_volume_parents[tet]);
      patch.connected_volume_boundary.push_back(
          topology.connected_volume_boundary[tet]);
      patch.connected_volume_regions.push_back(
          topology.connected_volume_regions[tet]);
    }
    optimize_connected_volume_boundary(patch,sphere,patch_distances);

    JobResult result;
    result.snapshot.id=job.id;
    result.snapshot.source_hierarchy_revision=directory.revision();
    result.snapshot.metrics.optimizer_passes=surface_optimizer_passes;
    result.snapshot.metrics.dependency_halo_rings=
        surface_optimizer_dependency_halo_rings;
    std::set<tetra::HierarchyBlockId> dependencies;
    for(const auto face:patch_faces)dependencies.insert(triangle_blocks[face]);
    for(const auto tet:patch_tets){
      const auto owner=logical_owner_by_cell.find(
          topology.connected_volume_parents[tet]);
      if(owner==logical_owner_by_cell.end())
        throw std::logic_error("blocked surface patch tetrahedron lacks an owner");
      const auto lookup=directory.lookup(owner->second);
      if(!lookup)throw std::logic_error("blocked surface dependency is not resident");
      dependencies.insert(lookup.block->id);
    }
    dependencies.erase(job.id);
    result.snapshot.dependency_blocks.assign(dependencies.begin(),dependencies.end());

    std::vector<std::size_t> core_vertices;
    for(const auto triangle:job.triangles)
      for(const auto vertex:boundary[triangle].vertices)core_vertices.push_back(vertex);
    std::ranges::sort(core_vertices,[&](std::size_t first,std::size_t second){
      return topology.connected_volume_global_keys[first]<
             topology.connected_volume_global_keys[second];
    });
    core_vertices.erase(std::unique(core_vertices.begin(),core_vertices.end()),
                        core_vertices.end());
    for(const auto global:core_vertices){
      const auto local=local_index[global];
      result.snapshot.vertices.push_back(
          {topology.connected_volume_global_keys[global],
           patch.connected_volume_vertices[local]});
    }
    for(const auto triangle:job.triangles){
      const auto& ids=boundary[triangle].vertices;
      result.snapshot.triangles.push_back({{{
          topology.connected_volume_global_keys[ids[0]],
          topology.connected_volume_global_keys[ids[1]],
          topology.connected_volume_global_keys[ids[2]]}},
          triangle_owners[triangle]});
    }
    result.snapshot.metrics.vertices=result.snapshot.vertices.size();
    result.snapshot.metrics.triangles=result.snapshot.triangles.size();
    result.snapshot.metrics.dependency_blocks=
        result.snapshot.dependency_blocks.size();
    result.core_vertices=core_vertices.size();
    result.patch_vertices=patch.connected_volume_vertices.size();
    result.patch_triangles=patch_faces.size();
    result.minimum_connected_tet_quality_after=
        patch.minimum_connected_tet_quality_after;
    for(const auto& tet:patch.connected_volume_tetrahedra){
      const auto volume=signed_six_volume(
          patch.connected_volume_vertices[tet[0]],
          patch.connected_volume_vertices[tet[1]],
          patch.connected_volume_vertices[tet[2]],
          patch.connected_volume_vertices[tet[3]]);
      if(!(volume>0.0)){result.connected_volume_valid=false;break;}
    }
    completed[job_index]=std::move(result);
  };

  const auto budget=std::min(options.operation_budget,
                             std::max<std::size_t>(1U,jobs.size()));
  std::size_t batches{};
  for(std::size_t begin=0;begin<jobs.size();begin+=budget){
    check_canceled();++batches;
    const auto end=std::min(jobs.size(),begin+budget);
    if(executor&&executor->worker_count()>1U&&end-begin>1U){
      auto group=executor->make_group(
          directory.revision(),tetra::GeometryTaskPriority::interactive);
      executor->parallel_for(group,begin,end,options.job_group_size,
          [&](std::size_t first,std::size_t last,std::stop_token stop){
            for(std::size_t job=first;job<last;++job){
              if(stop.stop_requested())return;
              build_job(job);
            }
          });
      executor->wait_and_help(group);
    }else for(std::size_t job=begin;job<end;++job)build_job(job);
  }
  check_canceled();

  BlockedDerivedSurfaceBuild result;
  result.metrics.block_generations=directory.block_generations();
  result.metrics.source_vertices=static_cast<std::size_t>(std::ranges::count(
      topology.connected_volume_surface_vertices,static_cast<std::uint8_t>(1U)));
  result.metrics.source_triangles=boundary.size();
  result.metrics.surface_blocks=completed.size();
  result.metrics.scheduling_batches=batches;
  result.metrics.worker_count=executor?executor->worker_count():1U;
  for(auto& job:completed){
    result.metrics.total_core_vertices+=job.core_vertices;
    result.metrics.total_patch_vertices+=job.patch_vertices;
    result.metrics.total_patch_triangles+=job.patch_triangles;
    result.metrics.dependency_block_references+=
        job.snapshot.dependency_blocks.size();
    result.metrics.maximum_patch_vertices=std::max(
        result.metrics.maximum_patch_vertices,job.patch_vertices);
    result.metrics.maximum_dependency_blocks=std::max(
        result.metrics.maximum_dependency_blocks,
        job.snapshot.dependency_blocks.size());
    result.metrics.minimum_connected_tet_quality_after=std::min(
        result.metrics.minimum_connected_tet_quality_after,
        job.minimum_connected_tet_quality_after);
    result.metrics.connected_volume_valid=
        result.metrics.connected_volume_valid&&job.connected_volume_valid;
    result.snapshots.push_back(std::move(job.snapshot));
  }
  const auto metrics=result.metrics;
  result=assemble_blocked_snapshots(std::move(result.snapshots));
  result.metrics=metrics;
  if(result.vertices.size()!=result.metrics.source_vertices||
     result.triangles.size()!=boundary.size())
    throw std::logic_error("blocked surface assembly is incomplete");
  result.metrics.halo_amplification=result.metrics.source_vertices==0U?0.0:
      static_cast<double>(result.metrics.total_patch_vertices)/
      static_cast<double>(result.metrics.source_vertices);
  result.metrics.build_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  return result;
}

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
      retained_patches_.capacity()*sizeof(RetainedPatch)+
      copy_scratch_.capacity()*sizeof(CopyJob);
  metrics_.pack_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-start).count();
}

void SurfaceDrawChunkStorage::compact(
    std::span<const SurfacePatchRecord> patches) {
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
      copy_scratch_.push_back({
          patch.triangle_begin+source_offset,destination,count});
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

void SurfaceDrawChunkStorage::run_copy_jobs(
    std::span<const tetra::Triangle> patch_arena,
    tetra::GeometryExecutor* executor) {
  const auto copy=[&](std::size_t begin,std::size_t end,std::stop_token stop){
    for(std::size_t index=begin;index<end;++index){
      if(stop.stop_requested())return;
      const auto& job=copy_scratch_[index];
      std::copy_n(
          patch_arena.begin()+static_cast<std::ptrdiff_t>(
              job.source_triangle_begin),job.triangle_count,
          arena_.begin()+static_cast<std::ptrdiff_t>(
              job.destination_triangle_begin));
    }
  };
  if(executor&&executor->worker_count()>1U&&copy_scratch_.size()>1U){
    const auto start=std::chrono::steady_clock::now();
    auto group=executor->make_group(
        next_content_revision_,tetra::GeometryTaskPriority::publication_critical);
    const std::size_t grain=std::max<std::size_t>(
        1U,(copy_scratch_.size()+executor->worker_count()-1U)/
            executor->worker_count());
    executor->parallel_for(group,0U,copy_scratch_.size(),grain,copy);
    executor->wait_and_help(group);
    metrics_.parallel_copy_tasks=(copy_scratch_.size()+grain-1U)/grain;
    metrics_.parallel_copy_milliseconds=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-start).count();
  }else copy(0U,copy_scratch_.size(),{});
}

void SurfaceDrawChunkStorage::pack(
    std::span<const SurfacePatchRecord> patches,
    std::span<const tetra::Triangle> patch_arena,
    tetra::GeometryExecutor* executor) {
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
  copy_scratch_.clear();
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
    compact(patches);
    run_copy_jobs(patch_arena,executor);
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
        copy_scratch_.push_back({
            patch.triangle_begin+segment.source_triangle_offset,
            segment.triangle_begin,segment.triangle_count});
        dirty_chunks[segment.chunk_index]=true;
        metrics_.copied_bytes+=segment.triangle_count*sizeof(tetra::Triangle);
      }
    }
    run_copy_jobs(patch_arena,executor);
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
    compact(patches);
    run_copy_jobs(patch_arena,executor);
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
    compact(patches);
    run_copy_jobs(patch_arena,executor);
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
      copy_scratch_.push_back({
          patch.triangle_begin+source_offset,destination,count});
      replacement_segments.push_back({patch.logical_owner,source_offset,
          replacement_chunks.size()-1U,destination,count});
      ++chunk.segment_count;
      chunk.triangle_count+=count;
      source_offset+=count;
    }
    previous_large_patch=large_patch;
  }

  run_copy_jobs(patch_arena,executor);

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
    std::span<const SceneVertex> logical_vertices,
    tetra::GeometryExecutor* executor) {
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

  std::vector<SourceView> sources;
  sources.reserve(source.chunks().size());
  logical_vertex_offset=0U;
  for(const auto& chunk:source.chunks()){
    const auto vertex_count=chunk.triangle_count*3U;
    sources.push_back({
        .key={.kind=SurfaceHostDrawRange::SourceKind::draw_chunk,
              .arena_slot=chunk.arena_slot},
        .content_revision=chunk.content_revision,
        .vertices=logical_vertices.subspan(logical_vertex_offset,vertex_count)});
    logical_vertex_offset+=vertex_count;
  }
  stage_sources(sources,executor);
}

void SurfaceHostStagingStorage::stage_world_render_blocks(
    std::span<const SparseWorldSurfaceCache::RenderBlock> blocks,
    tetra::GeometryExecutor* executor) {
  std::vector<SourceView> sources;
  std::size_t count{};
  for(const auto& block:blocks){
    if(block.surface_payload_hash==0U||block.triangle_vertices.size()%3U!=0U)
      throw std::invalid_argument("world render block is not triangle aligned");
    count+=(block.triangle_vertices.size()+vertex_slot_capacity()-1U)/
        vertex_slot_capacity();
  }
  sources.reserve(count);
  for(const auto& block:blocks){
    std::size_t begin{};std::uint32_t part{};
    while(begin<block.triangle_vertices.size()){
      const auto remaining=block.triangle_vertices.size()-begin;
      auto vertex_count=std::min(remaining,vertex_slot_capacity());
      vertex_count-=vertex_count%3U;
      if(vertex_count==0U||part==std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("world render block part overflow");
      sources.push_back({
          .key={.kind=SurfaceHostDrawRange::SourceKind::world_render_block,
                .block=block.id,.part=part++},
          .content_revision=block.surface_payload_hash,
          .vertices=std::span(block.triangle_vertices).subspan(
              begin,vertex_count)});
      begin+=vertex_count;
    }
  }
  stage_sources(sources,executor);
}

SurfaceHostStageEstimate SurfaceHostStagingStorage::estimate_world_render_blocks(
    std::span<const SparseWorldSurfaceCache::RenderBlock> blocks) const {
  SurfaceHostStageEstimate result;
  const auto add=[](std::size_t left,std::size_t right){
    if(right>std::numeric_limits<std::size_t>::max()-left)
      throw std::overflow_error("world render block estimate size overflow");
    return left+right;
  };
  const auto multiply=[](std::size_t left,std::size_t right){
    if(left!=0U&&right>std::numeric_limits<std::size_t>::max()/left)
      throw std::overflow_error("world render block estimate size overflow");
    return left*right;
  };
  using LookupKey=std::tuple<SurfaceHostDrawRange::SourceKey,std::uint64_t,
                             std::size_t>;
  std::map<LookupKey,std::size_t> previous;
  for(std::size_t index=0;index<ranges_.size();++index)
    previous.emplace(LookupKey{ranges_[index].source,
        ranges_[index].source_content_revision,
        ranges_[index].triangle_vertex_count},index);
  std::vector<bool> retained(ranges_.size(),false);
  for(const auto& block:blocks){
    if(block.surface_payload_hash==0U||block.triangle_vertices.size()%3U!=0U)
      throw std::invalid_argument("world render block is not triangle aligned");
    std::size_t begin{};std::uint32_t part{};
    while(begin<block.triangle_vertices.size()){
      auto count=std::min(block.triangle_vertices.size()-begin,
                          vertex_slot_capacity());
      count-=count%3U;
      if(count==0U||part==std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("world render block estimate overflow");
      const SurfaceHostDrawRange::SourceKey key{
          .kind=SurfaceHostDrawRange::SourceKind::world_render_block,
          .block=block.id,.part=part++};
      const auto found=previous.find(
          LookupKey{key,block.surface_payload_hash,count});
      bool reuse=false;
      if(found!=previous.end()&&!retained[found->second]){
        const auto& range=ranges_[found->second];
        reuse=std::memcmp(block.triangle_vertices.data()+begin,
            arena_.data()+range.triangle_vertex_begin,
            count*sizeof(SceneVertex))==0;
        if(reuse)retained[found->second]=true;
      }
      if(reuse)++result.reused_ranges;
      else{
        ++result.dirty_ranges;
        result.staged_bytes=add(
            result.staged_bytes,multiply(count,sizeof(SceneVertex)));
      }
      ++result.active_ranges;begin+=count;
    }
  }
  std::size_t free_slots{};
  for(const auto range:free_ranges_)free_slots=add(free_slots,range.slot_count);
  const auto appended=result.dirty_ranges>free_slots?
      result.dirty_ranges-free_slots:0U;
  auto retained_slots=add(arena_.size()/vertex_slot_capacity(),appended);
  const auto released=ranges_.size()-result.reused_ranges;
  const auto remaining_free=add(free_slots,released)-
      std::min(free_slots,result.dirty_ranges);
  result.compaction=retained_slots>4096U&&
      remaining_free>result.active_ranges/2U;
  if(result.compaction){
    retained_slots=result.active_ranges;result.reused_ranges=0U;
    result.dirty_ranges=result.active_ranges;result.staged_bytes=0U;
    for(const auto& block:blocks)
      result.staged_bytes=add(result.staged_bytes,multiply(
          block.triangle_vertices.size(),sizeof(SceneVertex)));
  }
  result.required_vertex_capacity=multiply(
      retained_slots,vertex_slot_capacity());
  result.retained_bytes=multiply(
      result.required_vertex_capacity,sizeof(SceneVertex));
  result.retained_bytes=add(result.retained_bytes,multiply(multiply(
      std::max(ranges_.capacity(),result.active_ranges),
      sizeof(SurfaceHostDrawRange)),2U));
  result.retained_bytes=add(result.retained_bytes,multiply(
      free_ranges_.capacity(),sizeof(SurfaceHostFreeRange)));
  result.retained_bytes=add(result.retained_bytes,multiply(
      std::max(copy_scratch_.capacity(),result.dirty_ranges),sizeof(CopyJob)));
  return result;
}

void SurfaceHostStagingStorage::stage_sources(
    std::span<const SourceView> sources,tetra::GeometryExecutor* executor) {
  const auto start=std::chrono::steady_clock::now();

  SurfaceHostStagingMetrics candidate_metrics;
  candidate_metrics.publication_generation=metrics_.publication_generation+1U;
  if(candidate_metrics.publication_generation==0U)
    candidate_metrics.publication_generation=1U;
  candidate_metrics.source_chunks=sources.size();
  std::vector<SurfaceHostFreeRange> candidate_free_ranges=free_ranges_;
  std::vector<bool> retained_ranges(ranges_.size(),false);
  range_scratch_.clear();
  range_scratch_.reserve(std::max(range_scratch_.capacity(),sources.size()));
  copy_scratch_.clear();
  copy_scratch_.reserve(std::max(copy_scratch_.capacity(),sources.size()));
  using LookupKey=std::tuple<SurfaceHostDrawRange::SourceKey,std::uint64_t,
                             std::size_t>;
  std::map<LookupKey,std::size_t> previous;
  for(std::size_t index=0;index<ranges_.size();++index)
    previous.emplace(LookupKey{ranges_[index].source,
        ranges_[index].source_content_revision,
        ranges_[index].triangle_vertex_count},index);
  for(std::size_t source_index=0;source_index<sources.size();++source_index){
    const auto& source=sources[source_index];
    const auto vertex_count=source.vertices.size();
    if(vertex_count==0U||vertex_count>vertex_slot_capacity()||
       vertex_count%3U!=0U||source.content_revision==0U)
      throw std::invalid_argument("surface host source range is invalid");
    std::size_t retained_index=ranges_.size();
    const auto found=previous.find(
        LookupKey{source.key,source.content_revision,vertex_count});
    if(found!=previous.end()&&!retained_ranges[found->second]){
      const auto& range=ranges_[found->second];
      if(std::memcmp(source.vertices.data(),
           arena_.data()+static_cast<std::ptrdiff_t>(range.triangle_vertex_begin),
           source.vertices.size_bytes())==0)
        retained_index=found->second;
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
      copy_scratch_.push_back({
          .source_index=source_index,
          .destination_vertex_begin=destination,
          .vertex_count=vertex_count});
      ++candidate_metrics.dirty_ranges;
      candidate_metrics.staged_triangle_bytes+=
          vertex_count*sizeof(SceneVertex);
      candidate_metrics.aliased_wire_bytes+=
          vertex_count*sizeof(SceneVertex);
    }
    const auto begin=host_slot*vertex_slot_capacity();
    range_scratch_.push_back({
        .host_slot=host_slot,
        .source=source.key,
        .source_content_revision=source.content_revision,
        .host_content_revision=host_content_revision,
        .triangle_vertex_begin=begin,
        .triangle_vertex_count=vertex_count,
        .wire_vertex_begin=begin,
        .wire_vertex_count=vertex_count});
  }

  const auto copy_job=[&](std::size_t begin,std::size_t end,std::stop_token stop){
    for(std::size_t index=begin;index<end;++index){
      if(stop.stop_requested())return;
      const auto& copy=copy_scratch_[index];
      std::copy_n(
          sources[copy.source_index].vertices.begin(),
          copy.vertex_count,
          arena_.begin()+static_cast<std::ptrdiff_t>(
              copy.destination_vertex_begin));
    }
  };
  if(executor&&executor->worker_count()>1U&&copy_scratch_.size()>1U){
    const auto copy_start=std::chrono::steady_clock::now();
    auto group=executor->make_group(
        candidate_metrics.publication_generation,
        tetra::GeometryTaskPriority::publication_critical);
    const std::size_t grain=std::max<std::size_t>(
        1U,(copy_scratch_.size()+executor->worker_count()-1U)/
            executor->worker_count());
    executor->parallel_for(group,0U,copy_scratch_.size(),grain,copy_job);
    executor->wait_and_help(group);
    candidate_metrics.parallel_copy_tasks=
        (copy_scratch_.size()+grain-1U)/grain;
    candidate_metrics.parallel_copy_milliseconds=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-copy_start).count();
  }else copy_job(0U,copy_scratch_.size(),{});

  for(std::size_t index=0;index<ranges_.size();++index){
    if(retained_ranges[index])continue;
    candidate_free_ranges.push_back({ranges_[index].host_slot,1U});
    ++candidate_metrics.released_slots;
  }
  normalize_free_ranges(candidate_free_ranges);
  std::size_t candidate_free_slots{};
  for(const auto range:candidate_free_ranges)
    candidate_free_slots+=range.slot_count;
  const auto retained_slots=arena_.size()/vertex_slot_capacity();
  // A large camera jump can replace nearly every range. Transactional
  // staging necessarily allocates the replacement before releasing the old
  // front, but keeping those released slots forever makes repeated
  // walk/teleport/reversal sequences grow toward the sum of every front.
  // Compact only large, badly fragmented arenas; ordinary local movement
  // keeps stable slots and therefore preserves partial-upload locality.
  if(retained_slots>4096U&&
     candidate_free_slots>range_scratch_.size()/2U){
    std::vector<SceneVertex> compacted(
        range_scratch_.size()*vertex_slot_capacity());
    for(std::size_t index=0;index<range_scratch_.size();++index){
      auto& range=range_scratch_[index];
      std::copy_n(arena_.begin()+static_cast<std::ptrdiff_t>(
                      range.triangle_vertex_begin),
                  range.triangle_vertex_count,
                  compacted.begin()+static_cast<std::ptrdiff_t>(
                      index*vertex_slot_capacity()));
      range.host_slot=index;
      range.triangle_vertex_begin=index*vertex_slot_capacity();
      range.wire_vertex_begin=range.triangle_vertex_begin;
      range.host_content_revision=next_content_revision_++;
      if(next_content_revision_==0U)next_content_revision_=1U;
    }
    arena_.swap(compacted);
    candidate_free_ranges.clear();
    candidate_metrics.dirty_ranges=range_scratch_.size();
    candidate_metrics.reused_ranges=0U;
    candidate_metrics.staged_triangle_bytes=0U;
    candidate_metrics.aliased_wire_bytes=0U;
    for(const auto& range:range_scratch_){
      candidate_metrics.staged_triangle_bytes+=
          range.triangle_vertex_count*sizeof(SceneVertex);
      candidate_metrics.aliased_wire_bytes+=
          range.triangle_vertex_count*sizeof(SceneVertex);
    }
  }
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
      copy_scratch_.capacity()*sizeof(CopyJob)+
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
    tetra::Vec3 minimum{},maximum{};
    if(range.triangle_vertex_count!=0U){
      const auto& first=host.arena()[range.triangle_vertex_begin].position;
      minimum=maximum={first[0],first[1],first[2]};
      for(std::size_t vertex=0;vertex<range.triangle_vertex_count;++vertex){
        const auto& position=
            host.arena()[range.triangle_vertex_begin+vertex].position;
        minimum.x=std::min(minimum.x,static_cast<double>(position[0]));
        minimum.y=std::min(minimum.y,static_cast<double>(position[1]));
        minimum.z=std::min(minimum.z,static_cast<double>(position[2]));
        maximum.x=std::max(maximum.x,static_cast<double>(position[0]));
        maximum.y=std::max(maximum.y,static_cast<double>(position[1]));
        maximum.z=std::max(maximum.z,static_cast<double>(position[2]));
      }
    }
    draw_scratch_.push_back({range.triangle_vertex_begin,
                             range.triangle_vertex_count,minimum,maximum});
  }
  for(std::size_t slot=0;slot<slot_scratch_.size();++slot)
    if(!active_slots[slot])slot_scratch_[slot]={};
  metrics_.upload_ranges=upload_scratch_.size();
  metrics_.draw_calls=draw_scratch_.size();
}

bool surface_draw_range_intersects_frustum(
    const SurfaceDeviceDrawRange& range,
    std::span<const float,16> matrix) noexcept {
  struct ClipPoint { double x{},y{},z{},w{}; };
  std::array<ClipPoint,8> points{};
  std::size_t index{};
  for(const double x:{range.minimum.x,range.maximum.x})
    for(const double y:{range.minimum.y,range.maximum.y})
      for(const double z:{range.minimum.z,range.maximum.z}){
        auto& point=points[index++];
        point.x=matrix[0]*x+matrix[4]*y+matrix[8]*z+matrix[12];
        point.y=matrix[1]*x+matrix[5]*y+matrix[9]*z+matrix[13];
        point.z=matrix[2]*x+matrix[6]*y+matrix[10]*z+matrix[14];
        point.w=matrix[3]*x+matrix[7]*y+matrix[11]*z+matrix[15];
        if(!std::isfinite(point.x)||!std::isfinite(point.y)||
           !std::isfinite(point.z)||!std::isfinite(point.w))return true;
      }
  const auto all_outside=[&](auto outside){
    return std::ranges::all_of(points,outside);
  };
  return !(
      all_outside([](const ClipPoint& p){return p.x < -p.w;})||
      all_outside([](const ClipPoint& p){return p.x >  p.w;})||
      all_outside([](const ClipPoint& p){return p.y < -p.w;})||
      all_outside([](const ClipPoint& p){return p.y >  p.w;})||
      all_outside([](const ClipPoint& p){return p.z <  0.0;})||
      all_outside([](const ClipPoint& p){return p.z >  p.w;}));
}

SurfaceDrawVisibility classify_surface_draw_visibility(
    std::span<const SurfaceDeviceDrawRange> ranges,
    std::span<const float,16> view_projection) noexcept {
  SurfaceDrawVisibility result;
  result.resident_ranges=ranges.size();
  for(const auto& range:ranges){
    result.resident_triangles+=range.vertex_count/3U;
    if(!surface_draw_range_intersects_frustum(range,view_projection))continue;
    ++result.submitted_ranges;
    result.submitted_triangles+=range.vertex_count/3U;
  }
  return result;
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
                            bool surface_override_is_owner_patches,
                                          tetra::GeometryExecutor* executor,
                                          std::stop_token cancellation) {
  PreparedScene scene;
  scene.render_origin=preparation.render_origin;
  if(cancellation.stop_requested())return scene;
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
    scene.triangle_vertices.push_back(make_scene_vertex(scene,point,colour,normal));
  };

  const auto statistics_start = std::chrono::steady_clock::now();
  scene.relations.resize(leaves.size());
  constexpr std::size_t parallel_classification_threshold=1024U;
  if(executor&&executor->worker_count()>1U&&
     leaves.size()>=parallel_classification_threshold){
    const auto parallel_start=std::chrono::steady_clock::now();
    auto group=executor->make_group(
        mesh.revision(),tetra::GeometryTaskPriority::interactive);
    const std::size_t block_count=std::max<std::size_t>(
        1U,executor->worker_count()*executor->configuration().blocks_per_worker);
    const std::size_t grain=std::max<std::size_t>(
        1U,(leaves.size()+block_count-1U)/block_count);
    executor->parallel_for(group,0U,leaves.size(),grain,
        [&](std::size_t begin,std::size_t end,std::stop_token stop){
          for(std::size_t index=begin;index<end;++index){
            if(stop.stop_requested()||cancellation.stop_requested())return;
            scene.relations[index]=tetra::classify_tetrahedron(
                mesh,leaves[index],sphere);
          }
        });
    executor->wait_and_help(group);
    if(cancellation.stop_requested())return {};
    scene.parallel_classification_milliseconds=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-parallel_start).count();
    scene.parallel_classification_tasks=(leaves.size()+grain-1U)/grain;
    scene.parallel_classification_workers=executor->worker_count();
  }else for(std::size_t index=0;index<leaves.size();++index){
      if((index&255U)==0U&&cancellation.stop_requested())return {};
      scene.relations[index]=tetra::classify_tetrahedron(
          mesh,leaves[index],sphere);
  }
  for (std::size_t leaf_index=0;leaf_index<leaves.size();++leaf_index) {
    if((leaf_index&255U)==0U&&cancellation.stop_requested())return {};
    const tetra::TetId id=leaves[leaf_index];
    if(preparation.summary_statistics){
      const auto depth = static_cast<std::size_t>(mesh.refinement_depth(id));
      if (scene.depth_counts.size() <= depth) scene.depth_counts.resize(depth + 1);
      ++scene.depth_counts[depth];
      scene.total_volume += mesh.signed_volume(id);
    }

    const auto relation=scene.relations[leaf_index];
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
    if((leaf_index&255U)==0U&&cancellation.stop_requested())return {};
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
        scene.hierarchy_line_vertices.push_back(
            make_scene_vertex(scene,point,colour));
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
  prepare_surface_render_attributes(scene,&sphere,executor);
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
  // Volume and connected-shell triangles are appended after surface
  // diagnostics. Finalize the complete draw list so every publication path,
  // including incremental cutaway updates, carries unit geometric normals.
  prepare_surface_render_attributes(scene,&sphere,executor);
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
    std::uint64_t field_revision,SurfaceMethod surface_method,
    tetra::GeometryExecutor* executor){
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

  struct ParallelPatchBlock {
    std::size_t dirty_begin{};
    std::size_t dirty_end{};
    std::vector<std::size_t> offsets;
    std::vector<tetra::Triangle> triangles;
  };
  std::vector<std::size_t> parallel_dirty_indices;
  std::vector<ParallelPatchBlock> parallel_patch_blocks;
  std::size_t parallel_patch_grain{};
  const bool owner_local_parallel=
      (surface_method==SurfaceMethod::marching_tetrahedra||
       surface_method==SurfaceMethod::lattice_cleaving)&&executor&&
      executor->worker_count()>1U;
  if(owner_local_parallel){
    parallel_dirty_indices.reserve(surface_patch_dirty_scratch_.size());
    for(std::size_t owner_index=0;
        owner_index<surface_patch_owner_scratch_.size();++owner_index){
      const auto owner=surface_patch_owner_scratch_[owner_index];
      const auto found=std::lower_bound(
          surface_patch_records_.begin(),surface_patch_records_.end(),owner,
          [](const auto& record,tetra::TetId value){
            return record.logical_owner<value;
          });
      const bool retained=found!=surface_patch_records_.end()&&
          found->logical_owner==owner;
      if(rebuild_all||!retained||std::binary_search(
             surface_patch_dirty_scratch_.begin(),
             surface_patch_dirty_scratch_.end(),owner))
        parallel_dirty_indices.push_back(owner_index);
    }
  }
  constexpr std::size_t parallel_patch_threshold=512U;
  if(owner_local_parallel&&
     parallel_dirty_indices.size()>=parallel_patch_threshold){
    const std::size_t block_limit=executor->worker_count()*
        executor->configuration().blocks_per_worker;
    parallel_patch_grain=std::max<std::size_t>(
        1U,(parallel_dirty_indices.size()+block_limit-1U)/block_limit);
    const std::size_t block_count=(parallel_dirty_indices.size()+
        parallel_patch_grain-1U)/parallel_patch_grain;
    parallel_patch_blocks.resize(block_count);
    const auto start_parallel=std::chrono::steady_clock::now();
    auto group=executor->make_group(
        mesh.revision(),tetra::GeometryTaskPriority::interactive);
    executor->parallel_for(
        group,0U,parallel_dirty_indices.size(),parallel_patch_grain,
        [&](std::size_t begin,std::size_t end,std::stop_token stop){
          auto& block=parallel_patch_blocks[begin/parallel_patch_grain];
          block.dirty_begin=begin;
          block.dirty_end=end;
          block.offsets.clear();
          block.triangles.clear();
          block.offsets.reserve(end-begin+1U);
          block.offsets.push_back(0U);
          std::vector<tetra::TetId> cells;
          std::vector<tetra::Triangle> triangles;
          for(std::size_t dirty_index=begin;dirty_index<end;++dirty_index){
            if(stop.stop_requested())return;
            const auto owner_index=parallel_dirty_indices[dirty_index];
            const auto owner=surface_patch_owner_scratch_[owner_index];
            cells.clear();
            triangles.clear();
            if(bcc){
              const auto offsets=mesh.logical_derived_offsets();
              const auto addresses=mesh.logical_derived_addresses();
              if(offsets.size()==surface_patch_owner_scratch_.size()+1U&&
                 offsets[owner_index]!=offsets[owner_index+1U])
                cells.assign(
                    addresses.begin()+static_cast<std::ptrdiff_t>(
                        offsets[owner_index]),
                    addresses.begin()+static_cast<std::ptrdiff_t>(
                        offsets[owner_index+1U]));
              else cells.push_back(owner);
            }else{
              const auto found=std::lower_bound(
                  surface_patch_owner_cells_.begin(),
                  surface_patch_owner_cells_.end(),owner,
                  [](const auto& entry,tetra::TetId value){
                    return entry.owner<value;
                  });
              for(auto cell=found;
                  cell!=surface_patch_owner_cells_.end()&&cell->owner==owner;
                  ++cell)cells.push_back(cell->cell);
            }
            std::sort(cells.begin(),cells.end());
            tetra::extract_isosurface(mesh,sphere,cells,triangles);
            block.triangles.insert(
                block.triangles.end(),triangles.begin(),triangles.end());
            block.offsets.push_back(block.triangles.size());
          }
        });
    executor->wait_and_help(group);
    surface_patch_metrics_.parallel_generation_milliseconds=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-start_parallel).count();
    surface_patch_metrics_.parallel_generation_tasks=block_count;
    surface_patch_metrics_.parallel_generation_workers=executor->worker_count();
  }
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
  std::size_t parallel_dirty_cursor{};
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
    if(!parallel_patch_blocks.empty()){
      if(parallel_dirty_cursor>=parallel_dirty_indices.size()||
         parallel_dirty_indices[parallel_dirty_cursor]!=owner_index)
        throw std::logic_error(
            "parallel surface patch stream lost owner ordering");
      const auto& block=parallel_patch_blocks[
          parallel_dirty_cursor/parallel_patch_grain];
      const auto local=parallel_dirty_cursor-block.dirty_begin;
      const auto begin=block.offsets[local];
      const auto end=block.offsets[local+1U];
      surface_patch_triangle_scratch_.assign(
          block.triangles.begin()+static_cast<std::ptrdiff_t>(begin),
          block.triangles.begin()+static_cast<std::ptrdiff_t>(end));
      ++parallel_dirty_cursor;
    }else if(dual_topology){
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
  if(!parallel_patch_blocks.empty()&&
     parallel_dirty_cursor!=parallel_dirty_indices.size())
    throw std::logic_error("parallel surface patches were not fully consumed");
  surface_patch_records_.swap(surface_patch_record_scratch_);
  if(four_hexahedra){
    four_hexahedra_patch_builder_.finish_update();
    const auto& field_metrics=four_hexahedra_patch_builder_.metrics();
    surface_patch_metrics_.evaluated_field_samples=field_metrics.evaluated_samples;
    surface_patch_metrics_.reused_field_samples=field_metrics.reused_samples;
    surface_patch_metrics_.field_sample_records=field_metrics.records;
  }
  if(mixed_depth_dual){
    const auto& field_metrics=mixed_depth_dual_patch_builder_.metrics();
    surface_patch_metrics_.evaluated_field_samples=field_metrics.evaluated_samples;
    surface_patch_metrics_.field_sample_records=field_metrics.flag_tetrahedra;
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
                              std::uint64_t surface_override_revision,
                              tetra::GeometryExecutor* executor) {
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
      update_surface_patches(
          mesh,sphere,sphere_revision,surface_method,executor);
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
  // The cached path assembles cutaway/connected-volume geometry after the
  // reusable base surface. Finalize that complete publication just like the
  // monolithic prepare_scene path; otherwise refined faces retain tiny raw
  // area vectors that the fragment shader can mistake for missing normals.
  prepare_surface_render_attributes(scene_,&sphere);
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
