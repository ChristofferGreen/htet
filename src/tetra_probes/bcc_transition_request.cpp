#include "tetra_probes/bcc_transition_request.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

namespace tetra::probes {
namespace {

using Face=std::array<WorldVertexKey,3>;
using LocalFace=std::array<std::uint32_t,3>;

Face canonical_face(Face face) {
  std::ranges::sort(face);
  return face;
}

std::uint64_t address_payload(WorldTetAddress owner,std::uint8_t local) {
  const auto depth=owner.red_depth();
  if(depth>16U||local>=4U)
    throw std::out_of_range("BCC transition stable-id address depth");
  const auto path_bits=depth*3U;
  const auto mask=path_bits==0U?std::uint64_t{0}:
      (std::uint64_t{1}<<path_bits)-1U;
  const auto prefix=static_cast<std::uint64_t>(owner.root_id())*17U+depth;
  return (((prefix<<48U)|(owner.low&mask))<<2U)|local;
}

std::uint64_t hierarchy_payload(WorldVertexKey key) {
  if(key.denominator_exponent>17U||key.x<0||key.y<0||key.z<0||
     key.x>0x3ffff||key.y>0x3ffff||key.z>0x3ffff)
    throw std::out_of_range("BCC transition stable-id vertex range");
  auto payload=static_cast<std::uint64_t>(key.denominator_exponent);
  payload=(payload<<18U)|static_cast<std::uint64_t>(key.x);
  payload=(payload<<18U)|static_cast<std::uint64_t>(key.y);
  payload=(payload<<18U)|static_cast<std::uint64_t>(key.z);
  return payload;
}

std::uint64_t surface_id(BccHexCellAddress address) {
  return (std::uint64_t{1}<<62U)|
      address_payload(address.owner,address.local_hexahedron);
}

std::uint64_t hierarchy_id(WorldVertexKey key) {
  return (std::uint64_t{2}<<62U)|hierarchy_payload(key);
}

std::uint64_t cap_id(BccHexCellAddress address) {
  return (std::uint64_t{3}<<62U)|
      address_payload(address.owner,address.local_hexahedron);
}

std::uint64_t collar_id(BccHexCellAddress address) {
  return address_payload(address.owner,address.local_hexahedron);
}

Vec3 probe_position(WorldVertexKey key) {
  const double denominator=std::ldexp(1.0,key.denominator_exponent);
  return {2.0*static_cast<double>(key.x)/denominator-1.0,
          2.0*static_cast<double>(key.y)/denominator-1.0,
          2.0*static_cast<double>(key.z)/denominator-1.0};
}

} // namespace

std::uint64_t bcc_transition_surface_vertex_id(BccHexCellAddress address) {
  return surface_id(address);
}

std::uint64_t bcc_transition_hierarchy_vertex_id(WorldVertexKey key) {
  return hierarchy_id(key);
}

BccScaffoldSurfacePartition partition_bcc_surface_over_transition_scaffold(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core) {
  BccScaffoldSurfacePartition result;
  result.source_triangles=surface.triangles.size();
  result.finite=true;
  result.barycentrics_valid=true;
  if(surface.red_depth!=core.red_depth||surface.triangles.empty()||
     surface.vertices.size()!=surface.vertex_owners.size()||
     !surface.validation.valid)return result;

  struct Point {
    Vec3 position;
    std::array<double,3> barycentric{};
  };
  struct Cell {
    WorldTetAddress owner;
    std::array<Vec3,4> vertices{};
    std::array<WorldVertexKey,4> vertex_keys{};
    Vec3 lower,upper;
  };
  const auto minimum=[](Vec3 a,Vec3 b) {
    return Vec3{std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)};
  };
  const auto maximum=[](Vec3 a,Vec3 b) {
    return Vec3{std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};
  };
  const auto cross=[](Vec3 a,Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  const auto area=[&](Vec3 a,Vec3 b,Vec3 c) {
    const auto n=cross(b-a,c-a);
    return 0.5*std::sqrt(dot(n,n));
  };
  const auto determinant=[&](const std::array<Vec3,4>& tet) {
    return dot(tet[1]-tet[0],cross(tet[2]-tet[0],tet[3]-tet[0]));
  };
  std::vector<WorldTetAddress> frontier;
  // Partition only the world roots named by this request. The structured
  // two-hexahedron wrapper deliberately supplies root zero; enumerating all
  // twelve cube roots and later interpreting them in root zero's affine frame
  // creates geometrically unrelated cut owners outside the parent tetrahedron.
  std::set<std::uint8_t> requested_roots;
  for(const auto& owner:surface.vertex_owners)
    requested_roots.insert(owner.owner.root_id());
  for(const auto owner:core.logical_owners)
    requested_roots.insert(owner.root_id());
  for(const auto root:requested_roots)
    frontier.push_back(WorldTetAddress::root(root));
  for(unsigned int depth=0U;depth<surface.red_depth;++depth) {
    std::vector<WorldTetAddress> children;
    children.reserve(frontier.size()*8U);
    for(const auto owner:frontier)for(std::uint8_t child=0U;child<8U;++child)
      children.push_back(owner.child(child));
    frontier.swap(children);
  }
  std::vector<Cell> cells;
  cells.reserve(frontier.size());
  for(const auto owner:frontier) {
    Cell cell;cell.owner=owner;
    const auto keys=world_tetrahedron_vertex_keys(owner);
    cell.vertex_keys=keys;
    for(std::size_t i=0U;i<4U;++i)cell.vertices[i]=probe_position(keys[i]);
    cell.lower=cell.upper=cell.vertices[0];
    for(std::size_t i=1U;i<4U;++i) {
      cell.lower=minimum(cell.lower,cell.vertices[i]);
      cell.upper=maximum(cell.upper,cell.vertices[i]);
    }
    cells.push_back(cell);
  }
  std::set<WorldTetAddress> retained(core.logical_owners.begin(),
                                      core.logical_owners.end());
  std::set<WorldTetAddress> cut;
  std::vector<std::size_t> fragments_per_source(surface.triangles.size());
  using FragmentKey=std::array<std::uint32_t,3>;
  std::vector<std::set<FragmentKey>> emitted(surface.triangles.size());
  std::map<BccScaffoldVertexKey,std::uint32_t> canonical_vertices;
  constexpr double epsilon=1.0e-12;
  constexpr double feature_epsilon=1.0e-9;
  const auto sort_prefix3=[](auto& values,std::uint8_t count) {
    if(count>=2U&&values[1]<values[0])std::swap(values[0],values[1]);
    if(count==3U) {
      if(values[2]<values[1])std::swap(values[1],values[2]);
      if(values[1]<values[0])std::swap(values[0],values[1]);
    }
  };
  result.canonical_keys_valid=true;
  result.canonical_positions_consistent=true;
  for(std::size_t source=0U;source<surface.triangles.size();++source) {
    const auto triangle=surface.triangles[source];
    if(std::ranges::any_of(triangle,[&](auto index){return index>=surface.vertices.size();})) {
      result.finite=false;continue;
    }
    std::array<Vec3,3> source_points{};
    Vec3 triangle_lower{},triangle_upper{};
    for(std::size_t corner=0U;corner<3U;++corner) {
      const auto& p=surface.vertices[triangle[corner]];
      source_points[corner]={p[0],p[1],p[2]};
      if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2]))
        result.finite=false;
      if(corner==0U)triangle_lower=triangle_upper=source_points[corner];
      else {
        triangle_lower=minimum(triangle_lower,source_points[corner]);
        triangle_upper=maximum(triangle_upper,source_points[corner]);
      }
    }
    const double parent_area=area(source_points[0],source_points[1],source_points[2]);
    result.source_area+=parent_area;
    for(const auto& cell:cells) {
      if(triangle_upper.x<cell.lower.x-epsilon||cell.upper.x<triangle_lower.x-epsilon||
         triangle_upper.y<cell.lower.y-epsilon||cell.upper.y<triangle_lower.y-epsilon||
         triangle_upper.z<cell.lower.z-epsilon||cell.upper.z<triangle_lower.z-epsilon)
        continue;
      std::vector<Point> polygon{
          {source_points[0],{{1.0,0.0,0.0}}},
          {source_points[1],{{0.0,1.0,0.0}}},
          {source_points[2],{{0.0,0.0,1.0}}}};
      for(std::size_t omitted=0U;omitted<4U&&!polygon.empty();++omitted) {
        std::array<std::size_t,3> face{};std::size_t cursor{};
        for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=i;
        const auto a=cell.vertices[face[0]];
        auto normal=cross(cell.vertices[face[1]]-a,cell.vertices[face[2]]-a);
        if(dot(normal,cell.vertices[omitted]-a)>0.0)normal=normal*-1.0;
        const auto signed_distance=[&](Vec3 point){return dot(normal,point-a);};
        std::vector<Point> clipped;
        for(std::size_t i=0U;i<polygon.size();++i) {
          const auto& first=polygon[i];
          const auto& second=polygon[(i+1U)%polygon.size()];
          const double first_distance=signed_distance(first.position);
          const double second_distance=signed_distance(second.position);
          const bool first_inside=first_distance<=epsilon;
          const bool second_inside=second_distance<=epsilon;
          if(first_inside)clipped.push_back(first);
          if(first_inside==second_inside)continue;
          const double t=first_distance/(first_distance-second_distance);
          Point intersection;
          intersection.position=first.position+(second.position-first.position)*t;
          for(std::size_t component=0U;component<3U;++component)
            intersection.barycentric[component]=first.barycentric[component]+
                (second.barycentric[component]-first.barycentric[component])*t;
          clipped.push_back(intersection);
        }
        polygon=std::move(clipped);
      }
      if(polygon.size()<3U)continue;
      for(std::size_t fan=1U;fan+1U<polygon.size();++fan) {
        const std::array<Point,3> fragment{{polygon[0],polygon[fan],polygon[fan+1U]}};
        const double fragment_area=area(fragment[0].position,fragment[1].position,
                                        fragment[2].position);
        if(fragment_area<=epsilon*std::max(1.0,parent_area))continue;
        BccScaffoldSurfaceTriangle output;
        output.owner=cell.owner;output.source_triangle=static_cast<std::uint32_t>(source);
        for(std::size_t corner=0U;corner<3U;++corner) {
          output.positions[corner]={{fragment[corner].position.x,
                                     fragment[corner].position.y,
                                     fragment[corner].position.z}};
          output.source_barycentrics[corner]=fragment[corner].barycentric;
          const auto sum=fragment[corner].barycentric[0]+fragment[corner].barycentric[1]+
                         fragment[corner].barycentric[2];
          result.barycentrics_valid=result.barycentrics_valid&&
              std::abs(sum-1.0)<=1.0e-9&&
              std::ranges::all_of(fragment[corner].barycentric,
                                  [](double value){return value>=-1.0e-9&&value<=1.0+1.0e-9;});

          BccScaffoldVertexKey key;
          for(std::size_t component=0U;component<3U;++component)
            if(fragment[corner].barycentric[component]>feature_epsilon)
              key.surface_vertices[key.surface_vertex_count++]=triangle[component];
          sort_prefix3(key.surface_vertices,key.surface_vertex_count);

          if(key.surface_vertex_count!=1U) {
            const double cell_determinant=determinant(cell.vertices);
            if(std::abs(cell_determinant)<=epsilon) {
              result.canonical_keys_valid=false;
            } else {
              for(std::size_t component=0U;component<4U;++component) {
                auto replaced=cell.vertices;
                replaced[component]=fragment[corner].position;
                const double weight=determinant(replaced)/cell_determinant;
                if(weight>feature_epsilon) {
                  if(key.bcc_vertex_count<key.bcc_vertices.size())
                    key.bcc_vertices[key.bcc_vertex_count++]=cell.vertex_keys[component];
                  else result.canonical_keys_valid=false;
                } else if(weight<-feature_epsilon)result.canonical_keys_valid=false;
              }
              sort_prefix3(key.bcc_vertices,key.bcc_vertex_count);
            }
          }
          // Clipping can only introduce DC-edge/BCC-boundary or
          // DC-triangle/BCC-edge-or-vertex intersections.
          if(key.surface_vertex_count==0U||key.surface_vertex_count>3U||
             (key.surface_vertex_count==2U&&key.bcc_vertex_count>3U)||
             (key.surface_vertex_count==3U&&key.bcc_vertex_count>2U))
            result.canonical_keys_valid=false;

          const std::array<double,3> canonical_position{{fragment[corner].position.x,
                                                         fragment[corner].position.y,
                                                         fragment[corner].position.z}};
          const auto [canonical_it,inserted]=canonical_vertices.emplace(
              key,static_cast<std::uint32_t>(result.vertices.size()));
          if(inserted)result.vertices.push_back({key,canonical_position});
          else {
            const auto& previous=result.vertices[canonical_it->second].position;
            const double delta=std::max({std::abs(previous[0]-canonical_position[0]),
                                         std::abs(previous[1]-canonical_position[1]),
                                         std::abs(previous[2]-canonical_position[2])});
            if(delta>1.0e-8)result.canonical_positions_consistent=false;
          }
          output.canonical_vertex_indices[corner]=canonical_it->second;
        }
        FragmentKey fragment_key=output.canonical_vertex_indices;
        std::sort(fragment_key.begin(),fragment_key.end());
        if(!emitted[source].insert(fragment_key).second) {
          ++result.duplicate_coplanar_fragments;continue;
        }
        result.triangles.push_back(output);
        result.fragment_area+=fragment_area;
        ++fragments_per_source[source];cut.insert(cell.owner);
      }
    }
  }
  // Indices are the rank of the canonical key, rather than discovery order.
  // Thus a chunk can emit the same IDs regardless of its local traversal.
  std::vector<std::uint32_t> old_to_new(result.vertices.size());
  std::vector<BccScaffoldVertex> sorted_vertices;
  sorted_vertices.reserve(result.vertices.size());
  for(const auto& [key,old_index]:canonical_vertices) {
    old_to_new[old_index]=static_cast<std::uint32_t>(sorted_vertices.size());
    sorted_vertices.push_back(result.vertices[old_index]);
  }
  result.vertices=std::move(sorted_vertices);
  for(auto& triangle:result.triangles)
    for(auto& index:triangle.canonical_vertex_indices)index=old_to_new[index];

  using CanonicalEdge=std::array<std::uint32_t,2>;
  std::map<CanonicalEdge,std::vector<WorldTetAddress>> edge_owners;
  for(const auto& triangle:result.triangles)
    for(std::size_t edge=0U;edge<3U;++edge) {
      CanonicalEdge key{{triangle.canonical_vertex_indices[edge],
                         triangle.canonical_vertex_indices[(edge+1U)%3U]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      edge_owners[key].push_back(triangle.owner);
    }
  std::map<std::array<std::uint32_t,2>,std::size_t> source_edge_uses;
  for(const auto& triangle:surface.triangles)
    for(std::size_t edge=0U;edge<3U;++edge) {
      std::array<std::uint32_t,2> key{{triangle[edge],triangle[(edge+1U)%3U]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      ++source_edge_uses[key];
    }
  std::set<std::array<std::uint32_t,2>> source_boundary;
  for(const auto& [edge,count]:source_edge_uses)if(count==1U)source_boundary.insert(edge);
  result.canonical_edges=edge_owners.size();
  result.canonical_edge_incidence=true;
  result.source_boundary_preserved=true;
  std::set<WorldTetAddress> source_boundary_owners;
  for(const auto& [edge,owners]:edge_owners) {
    if(owners.empty()||owners.size()>2U)result.canonical_edge_incidence=false;
    if(owners.size()==2U&&owners[0]!=owners[1])++result.shared_owner_edges;
    if(owners.size()!=1U)continue;
    ++result.boundary_edges;
    std::set<std::uint32_t> supporting_vertices;
    for(const auto index:edge) {
      const auto& vertex_key=result.vertices[index].key;
      for(std::size_t i=0U;i<vertex_key.surface_vertex_count;++i)
        supporting_vertices.insert(vertex_key.surface_vertices[i]);
    }
    if(supporting_vertices.size()!=2U) {
      result.source_boundary_preserved=false;
      continue;
    }
    const std::array<std::uint32_t,2> supporting_edge{{*supporting_vertices.begin(),
                                                       *supporting_vertices.rbegin()}};
    if(!source_boundary.contains(supporting_edge)) {
      result.source_boundary_preserved=false;
    } else {
      source_boundary_owners.insert(owners.begin(),owners.end());
    }
  }
  for(const auto count:fragments_per_source) {
    if(count>0U)++result.covered_source_triangles;
    result.maximum_fragments_per_source=std::max(result.maximum_fragments_per_source,count);
  }
  result.cut_owners.assign(cut.begin(),cut.end());
  result.source_boundary_owners.assign(source_boundary_owners.begin(),
                                       source_boundary_owners.end());
  result.area_error=std::abs(result.source_area-result.fragment_area);
  result.exact_coverage=result.covered_source_triangles==result.source_triangles&&
      result.area_error<=1.0e-8*std::max(1.0,result.source_area);
  result.disjoint_from_retained_core=std::ranges::none_of(
      result.cut_owners,[&](WorldTetAddress owner){return retained.contains(owner);});
  result.valid=result.finite&&result.barycentrics_valid&&result.canonical_keys_valid&&
      result.canonical_positions_consistent&&result.canonical_edge_incidence&&
      result.source_boundary_preserved&&result.exact_coverage&&
      result.disjoint_from_retained_core;
  return result;
}

BccScaffoldSurfacePartition partition_structured_surface_over_global_core(
    const StructuredTwoHexDualSurface& surface) {
  if(surface.global_core_red_depth==0U||surface.dual_vertices.empty()||
     surface.triangles.empty()||
     (!surface.validation.valid&&!surface.has_dc_ghost_halo))
    return {};
  const auto root=WorldTetAddress::root(0U);
  const auto root_keys=world_tetrahedron_vertex_keys(root);
  std::array<Vec3,4> reference{};
  std::array<Vec3,4> parent{};
  for(std::size_t corner=0U;corner<4U;++corner) {
    reference[corner]=probe_position(root_keys[corner]);
    const auto& p=surface.parent_tetrahedron[corner];
    parent[corner]={p[0],p[1],p[2]};
  }
  const auto barycentric=[](const std::array<Vec3,4>& tet,Vec3 point) {
    const auto a=tet[1]-tet[0],b=tet[2]-tet[0],c=tet[3]-tet[0];
    const auto d=point-tet[0];
    const auto cross=[](Vec3 x,Vec3 y) {
      return Vec3{x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x};
    };
    const auto dot=[](Vec3 x,Vec3 y) {return x.x*y.x+x.y*y.y+x.z*y.z;};
    const double determinant=dot(a,cross(b,c));
    if(std::abs(determinant)<=1.0e-15)
      throw std::logic_error("degenerate structured hierarchy frame");
    const double u=dot(d,cross(b,c))/determinant;
    const double v=dot(a,cross(d,c))/determinant;
    const double w=dot(a,cross(b,d))/determinant;
    return std::array<double,4>{{1.0-u-v-w,u,v,w}};
  };
  const auto map=[](const std::array<Vec3,4>& target,
                    const std::array<double,4>& weights) {
    Vec3 result{};
    for(std::size_t corner=0U;corner<4U;++corner)
      result=result+target[corner]*weights[corner];
    return result;
  };
  const auto to_reference=[&](Vec3 point) {
    return map(reference,barycentric(parent,point));
  };
  const auto to_parent=[&](Vec3 point) {
    return map(parent,barycentric(reference,point));
  };

  BccHierarchyDualSurface normalized;
  normalized.red_depth=surface.global_core_red_depth;
  normalized.vertices.reserve(surface.dual_vertices.size());
  for(const auto& p:surface.dual_vertices) {
    const auto mapped=to_reference({p[0],p[1],p[2]});
    normalized.vertices.push_back({mapped.x,mapped.y,mapped.z});
  }
  normalized.triangles=surface.triangles;
  normalized.validation=surface.validation;
  // A sufficiently wide Cartesian ghost sheet can fold outside the finite
  // root tetrahedron even though the portion intersecting the root is clean.
  // The partition below is the exact root crop and performs its own canonical
  // incidence/coverage audit, so exterior-only halo defects must not prevent
  // that scoped audit from running.
  if(surface.has_dc_ghost_halo)normalized.validation.valid=true;
  normalized.vertex_owners.resize(normalized.vertices.size(),{root,0U});
  FrozenBccHierarchyCore core;
  core.red_depth=surface.global_core_red_depth;
  core.logical_owners=surface.global_core_tet_addresses;
  auto result=partition_bcc_surface_over_transition_scaffold(normalized,core);
  if(surface.has_dc_ghost_halo&&result.covered_source_triangles>0U) {
    // Ghost DC exists only to provide the neighbors of chunk-owned surface
    // cells. The transition domain is root zero; source triangles outside it
    // are neither missing terrain nor transition work. The leaf clipping
    // above is the exact domain crop, and boundary edges created by that crop
    // lie on root faces rather than on the artificial outer edge of the ghost
    // grid.
    result.source_triangles=result.covered_source_triangles;
    result.source_area=result.fragment_area;
    result.area_error=0.0;
    result.exact_coverage=true;
    result.source_boundary_preserved=true;
    // Keep the canonical owners touched by the finite ghost sheet's outer
    // boundary.  The sheet is neighborhood input, not an infinite surface;
    // callers need these labels to avoid publishing halo-edge cut cells as
    // complete transition volume.
    result.valid=result.finite&&result.barycentrics_valid&&
        result.canonical_keys_valid&&result.canonical_positions_consistent&&
        result.canonical_edge_incidence&&result.exact_coverage&&
        result.disjoint_from_retained_core;
  }
  for(auto& vertex:result.vertices) {
    const auto mapped=to_parent(
        {vertex.position[0],vertex.position[1],vertex.position[2]});
    vertex.position={{mapped.x,mapped.y,mapped.z}};
  }
  for(auto& triangle:result.triangles)
    for(auto& p:triangle.positions) {
      const auto mapped=to_parent({p[0],p[1],p[2]});
      p={{mapped.x,mapped.y,mapped.z}};
    }
  return result;
}

BccScaffoldConvexCellReport inspect_structured_global_cut_cells(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface) {
  BccScaffoldConvexCellReport result;
  const auto partition=partition_structured_surface_over_global_core(surface);
  result.partition_valid=partition.valid;
  result.cut_owners=partition.cut_owners.size();
  result.surface_fragments=partition.triangles.size();
  if(!partition.valid)return result;
  const auto reference=world_tetrahedron_geometry(WorldTetAddress::root(0U));
  std::array<Vec3,4> parent{};
  for(std::size_t corner=0U;corner<4U;++corner) {
    const auto& p=surface.parent_tetrahedron[corner];
    parent[corner]={p[0],p[1],p[2]};
  }
  const auto cross=[](Vec3 a,Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto transform=[&](::tetra::Vec3 point) {
    const Vec3 origin{reference[0].x,reference[0].y,reference[0].z};
    const Vec3 a{reference[1].x-origin.x,reference[1].y-origin.y,
                 reference[1].z-origin.z};
    const Vec3 b{reference[2].x-origin.x,reference[2].y-origin.y,
                 reference[2].z-origin.z};
    const Vec3 c{reference[3].x-origin.x,reference[3].y-origin.y,
                 reference[3].z-origin.z};
    const Vec3 d{point.x-origin.x,point.y-origin.y,point.z-origin.z};
    const double determinant=dot(a,cross(b,c));
    const double u=dot(d,cross(b,c))/determinant;
    const double v=dot(a,cross(d,c))/determinant;
    const double w=dot(a,cross(b,d))/determinant;
    const std::array<double,4> weights{{1.0-u-v-w,u,v,w}};
    Vec3 mapped{};
    for(std::size_t corner=0U;corner<4U;++corner)
      mapped=mapped+parent[corner]*weights[corner];
    return mapped;
  };
  constexpr double plane_epsilon=1.0e-9;
  std::map<WorldTetAddress,
           std::vector<const BccScaffoldSurfaceTriangle*>> fragments;
  for(const auto& fragment:partition.triangles)
    fragments[fragment.owner].push_back(&fragment);
  for(const auto owner:partition.cut_owners) {
    std::vector<Vec3> material_points;
    for(const auto point:world_tetrahedron_geometry(owner)) {
      const auto mapped=transform(point);
      if(evaluate_sandwich_field(config,{mapped.x,mapped.y,mapped.z})<=
         plane_epsilon)
        material_points.push_back(mapped);
    }
    const bool has_inside_vertex=!material_points.empty();
    if(has_inside_vertex)++result.owners_with_inside_vertex;
    std::set<std::uint32_t> arrangement_vertices;
    for(const auto* fragment:fragments[owner])
      arrangement_vertices.insert(fragment->canonical_vertex_indices.begin(),
                                  fragment->canonical_vertex_indices.end());
    for(const auto index:arrangement_vertices) {
      const auto& point=partition.vertices[index].position;
      material_points.push_back({point[0],point[1],point[2]});
    }
    result.maximum_material_vertices=std::max(result.maximum_material_vertices,
                                               material_points.size());
    bool owner_convex=has_inside_vertex;
    for(const auto* fragment:fragments[owner]) {
      const Vec3 a{fragment->positions[0][0],fragment->positions[0][1],
                   fragment->positions[0][2]};
      const Vec3 b{fragment->positions[1][0],fragment->positions[1][1],
                   fragment->positions[1][2]};
      const Vec3 c{fragment->positions[2][0],fragment->positions[2][1],
                   fragment->positions[2][2]};
      const auto normal=cross(b-a,c-a);
      double minimum=std::numeric_limits<double>::infinity();
      double maximum=-std::numeric_limits<double>::infinity();
      for(const auto point:material_points) {
        const double distance=dot(normal,point-a);
        minimum=std::min(minimum,distance);
        maximum=std::max(maximum,distance);
      }
      const double scale=std::max(1.0,std::sqrt(dot(normal,normal)));
      const bool supporting=minimum>=-plane_epsilon*scale||
                            maximum<=plane_epsilon*scale;
      if(supporting)++result.supporting_surface_fragments;
      else owner_convex=false;
    }
    if(owner_convex)++result.convex_material_owners;
    else ++result.nonconvex_material_owners;
  }
  result.complete_convex_route=result.partition_valid&&result.cut_owners>0U&&
      result.convex_material_owners==result.cut_owners;
  return result;
}

BccScaffoldConvexCellReport inspect_bcc_scaffold_convex_material_cells(
    const SandwichConfig& config,
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core) {
  BccScaffoldConvexCellReport result;
  const auto partition=partition_bcc_surface_over_transition_scaffold(surface,core);
  result.partition_valid=partition.valid;
  result.cut_owners=partition.cut_owners.size();
  result.surface_fragments=partition.triangles.size();
  if(!partition.valid)return result;
  const auto cross=[](Vec3 a,Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  constexpr double plane_epsilon=1.0e-9;
  std::map<WorldTetAddress,std::vector<const BccScaffoldSurfaceTriangle*>> fragments;
  for(const auto& fragment:partition.triangles)fragments[fragment.owner].push_back(&fragment);
  for(const auto owner:partition.cut_owners) {
    std::vector<Vec3> material_points;
    const auto keys=world_tetrahedron_vertex_keys(owner);
    for(const auto key:keys) {
      const auto point=probe_position(key);
      if(evaluate_sandwich_field(config,{point.x,point.y,point.z})<=plane_epsilon)
        material_points.push_back(point);
    }
    const bool has_inside_vertex=!material_points.empty();
    if(has_inside_vertex)++result.owners_with_inside_vertex;
    std::set<std::uint32_t> arrangement_vertices;
    for(const auto* fragment:fragments[owner])
      arrangement_vertices.insert(fragment->canonical_vertex_indices.begin(),
                                  fragment->canonical_vertex_indices.end());
    for(const auto index:arrangement_vertices) {
      const auto& point=partition.vertices[index].position;
      material_points.push_back({point[0],point[1],point[2]});
    }
    result.maximum_material_vertices=std::max(result.maximum_material_vertices,
                                               material_points.size());
    bool owner_convex=has_inside_vertex;
    for(const auto* fragment:fragments[owner]) {
      const Vec3 a{fragment->positions[0][0],fragment->positions[0][1],
                   fragment->positions[0][2]};
      const Vec3 b{fragment->positions[1][0],fragment->positions[1][1],
                   fragment->positions[1][2]};
      const Vec3 c{fragment->positions[2][0],fragment->positions[2][1],
                   fragment->positions[2][2]};
      const auto normal=cross(b-a,c-a);
      double minimum=std::numeric_limits<double>::infinity();
      double maximum=-std::numeric_limits<double>::infinity();
      for(const auto point:material_points) {
        const double distance=dot(normal,point-a);
        minimum=std::min(minimum,distance);
        maximum=std::max(maximum,distance);
      }
      const double scale=std::max(1.0,std::sqrt(dot(normal,normal)));
      const bool supporting=minimum>=-plane_epsilon*scale||
                            maximum<=plane_epsilon*scale;
      if(supporting)++result.supporting_surface_fragments;
      else owner_convex=false;
    }
    if(owner_convex)++result.convex_material_owners;
    else ++result.nonconvex_material_owners;
  }
  result.complete_convex_route=result.partition_valid&&result.cut_owners>0U&&
      result.convex_material_owners==result.cut_owners;
  return result;
}

namespace {
BccScaffoldTransitionVolume construct_scaffold_convex_transition(
    const BccScaffoldSurfacePartition& partition,
    unsigned int red_depth,
    std::span<const WorldTetAddress> retained_owners,
    std::span<const WorldTetAddress> hierarchy_roots,
    const std::function<Vec3(WorldVertexKey)>& hierarchy_position,
    const std::function<double(Vec3)>& field,
    const std::function<bool(const std::array<std::uint32_t,3>&,
                             const std::vector<std::array<double,3>>&)>&
        is_domain_boundary,
    const std::function<bool(const std::array<std::uint32_t,3>&,
                             const std::vector<std::array<double,3>>&)>&
        is_chunk_interface,
    bool convex_route_applicable) {
  BccScaffoldTransitionVolume result;
  result.convex_route_applicable=convex_route_applicable;
  if(!result.convex_route_applicable)return result;
  result.vertices.reserve(partition.vertices.size()+partition.cut_owners.size()*8U);
  for(const auto& vertex:partition.vertices)result.vertices.push_back(vertex.position);
  const auto cross=[](Vec3 a,Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  const auto six_volume=[&](const std::array<std::uint32_t,4>& tet) {
    const auto make=[&](std::uint32_t i) {
      const auto& p=result.vertices[i];return Vec3{p[0],p[1],p[2]};
    };
    const auto a=make(tet[0]),b=make(tet[1]),c=make(tet[2]),d=make(tet[3]);
    return dot(b-a,cross(c-a,d-a));
  };
  std::map<WorldVertexKey,std::uint32_t> hierarchy_indices;
  const auto hierarchy_index=[&](WorldVertexKey key) {
    const auto found=hierarchy_indices.find(key);
    if(found!=hierarchy_indices.end())return found->second;
    const auto point=hierarchy_position(key);
    const auto index=static_cast<std::uint32_t>(result.vertices.size());
    result.vertices.push_back({point.x,point.y,point.z});
    hierarchy_indices.emplace(key,index);return index;
  };
  std::map<WorldTetAddress,std::vector<const BccScaffoldSurfaceTriangle*>> fragments;
  for(const auto& fragment:partition.triangles)fragments[fragment.owner].push_back(&fragment);
  std::map<std::array<WorldVertexKey,3>,std::uint32_t> face_centres;
  std::vector<std::array<std::uint32_t,3>> transition_boundary;
  result.positive=true;
  constexpr double plane_epsilon=1.0e-8;
  for(const auto owner:partition.cut_owners) {
    const auto open_edges_before=result.local_boundary_open_edges;
    const auto owner_keys=world_tetrahedron_vertex_keys(owner);
    const auto owner_points=std::array<Vec3,4>{{hierarchy_position(owner_keys[0]),
                                                hierarchy_position(owner_keys[1]),
                                                hierarchy_position(owner_keys[2]),
                                                hierarchy_position(owner_keys[3])}};
    std::set<std::uint32_t> material_indices;
    for(std::size_t i=0U;i<4U;++i)
      if(field(owner_points[i])<=plane_epsilon)
        material_indices.insert(hierarchy_index(owner_keys[i]));
    std::vector<std::array<std::uint32_t,3>> boundary;
    for(const auto* fragment:fragments[owner]) {
      boundary.push_back(fragment->canonical_vertex_indices);
      result.exact_surface_triangles.push_back(fragment->canonical_vertex_indices);
      material_indices.insert(fragment->canonical_vertex_indices.begin(),
                              fragment->canonical_vertex_indices.end());
    }
    const std::size_t surface_boundary_faces=boundary.size();
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      std::array<std::size_t,3> face_corners{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face_corners[cursor++]=i;
      const auto a=owner_points[face_corners[0]];
      const auto normal=cross(owner_points[face_corners[1]]-a,
                              owner_points[face_corners[2]]-a);
      std::vector<std::uint32_t> face_points;
      for(const auto index:material_indices) {
        const auto& p=result.vertices[index];
        if(std::abs(dot(normal,Vec3{p[0],p[1],p[2]}-a))<=
           plane_epsilon*std::max(1.0,std::sqrt(dot(normal,normal))))
          face_points.push_back(index);
      }
      if(face_points.size()<3U)continue;
      const auto absolute=Vec3{std::abs(normal.x),std::abs(normal.y),std::abs(normal.z)};
      const unsigned int drop=absolute.x>=absolute.y&&absolute.x>=absolute.z?0U:
                              absolute.y>=absolute.z?1U:2U;
      const auto project=[&](std::uint32_t index) {
        const auto& p=result.vertices[index];
        return drop==0U?std::array<double,2>{{p[1],p[2]}}:
               drop==1U?std::array<double,2>{{p[0],p[2]}}:
                         std::array<double,2>{{p[0],p[1]}};
      };
      std::sort(face_points.begin(),face_points.end(),[&](auto lhs,auto rhs) {
        const auto l=project(lhs),r=project(rhs);
        return std::tie(l[0],l[1],lhs)<std::tie(r[0],r[1],rhs);
      });
      face_points.erase(std::unique(face_points.begin(),face_points.end()),face_points.end());
      const auto turn=[&](std::uint32_t first,std::uint32_t second,std::uint32_t third) {
        const auto p=project(first),q=project(second),r=project(third);
        return (q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]);
      };
      std::vector<std::uint32_t> lower,upper;
      for(const auto point:face_points) {
        while(lower.size()>=2U&&turn(lower[lower.size()-2U],lower.back(),point)<=1.0e-12)
          lower.pop_back();
        lower.push_back(point);
      }
      for(auto it=face_points.rbegin();it!=face_points.rend();++it) {
        while(upper.size()>=2U&&turn(upper[upper.size()-2U],upper.back(),*it)<=1.0e-12)
          upper.pop_back();
        upper.push_back(*it);
      }
      if(lower.size()+upper.size()<5U)continue;
      lower.pop_back();upper.pop_back();lower.insert(lower.end(),upper.begin(),upper.end());
      const auto hull=lower;
      std::vector<std::uint32_t> ring;
      for(std::size_t edge=0U;edge<hull.size();++edge) {
        const auto first=hull[edge],second=hull[(edge+1U)%hull.size()];
        const auto p=project(first),q=project(second);
        const double length2=(q[0]-p[0])*(q[0]-p[0])+(q[1]-p[1])*(q[1]-p[1]);
        std::vector<std::pair<double,std::uint32_t>> on_edge;
        for(const auto candidate:face_points) {
          const auto r=project(candidate);
          const double area2=(q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]);
          const double parameter=((r[0]-p[0])*(q[0]-p[0])+
                                  (r[1]-p[1])*(q[1]-p[1]))/length2;
          // These points were produced by independent DC-triangle/tet-face
          // clipping operations.  Their plane test above is scaled by
          // `plane_epsilon`; use the same scale when restoring collinear
          // subdivisions of a convex-hull edge.  A tighter absolute test
          // skipped canonical surface vertices and left a long hierarchy
          // edge opposite several shorter frozen-surface edges.
          if(std::abs(area2)<=plane_epsilon*std::sqrt(length2)&&
             parameter>=-plane_epsilon&&parameter<1.0-plane_epsilon)
            on_edge.emplace_back(parameter,candidate);
        }
        std::sort(on_edge.begin(),on_edge.end());
        for(const auto& [parameter,index]:on_edge) {
          (void)parameter;
          if(ring.empty()||ring.back()!=index)ring.push_back(index);
        }
      }
      if(ring.size()==3U)boundary.push_back({{ring[0],ring[1],ring[2]}});
      else if(ring.size()>3U) {
        std::array<WorldVertexKey,3> face_key{{owner_keys[face_corners[0]],
                                               owner_keys[face_corners[1]],
                                               owner_keys[face_corners[2]]}};
        std::sort(face_key.begin(),face_key.end());
        auto centre_it=face_centres.find(face_key);
        std::uint32_t centre{};
        if(centre_it==face_centres.end()) {
          std::array<double,3> position{};
          for(const auto index:ring)for(std::size_t axis=0U;axis<3U;++axis)
            position[axis]+=result.vertices[index][axis]/static_cast<double>(ring.size());
          centre=static_cast<std::uint32_t>(result.vertices.size());
          result.vertices.push_back(position);face_centres.emplace(face_key,centre);
        } else centre=centre_it->second;
        material_indices.insert(centre);
        for(std::size_t edge=0U;edge<ring.size();++edge)
          boundary.push_back({{centre,ring[edge],ring[(edge+1U)%ring.size()]}});
      }
    }
    std::array<double,3> centroid{};
    for(const auto index:material_indices)for(std::size_t axis=0U;axis<3U;++axis)
      centroid[axis]+=result.vertices[index][axis]/static_cast<double>(material_indices.size());
    const auto centre=static_cast<std::uint32_t>(result.vertices.size());
    result.vertices.push_back(centroid);
    std::map<std::array<std::uint32_t,2>,std::size_t> local_edge_uses;
    std::map<std::array<std::uint32_t,2>,std::uint8_t> local_edge_sources;
    for(std::size_t face_index=0U;face_index<boundary.size();++face_index)
      for(std::size_t edge=0U;edge<3U;++edge) {
      const auto& face=boundary[face_index];
      std::array<std::uint32_t,2> key{{face[edge],face[(edge+1U)%3U]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      ++local_edge_uses[key];
      local_edge_sources[key]|=face_index<surface_boundary_faces?1U:2U;
    }
    std::vector<std::array<std::uint32_t,2>> open_surface_edges;
    std::vector<std::array<std::uint32_t,2>> open_hierarchy_edges;
    for(const auto& [edge,count]:local_edge_uses) {
      if(count==2U)continue;
      ++result.local_boundary_open_edges;
      const auto geometry=[&]() {
        const auto& a=result.vertices[edge[0]];
        const auto& b=result.vertices[edge[1]];
        return std::array<double,6>{{a[0],a[1],a[2],b[0],b[1],b[2]}};
      };
      if((local_edge_sources[edge]&1U)!=0U) {
        open_surface_edges.push_back(edge);
        ++result.unmatched_surface_edges;
        if(result.unmatched_surface_edge_geometry.size()<16U)
          result.unmatched_surface_edge_geometry.push_back(geometry());
      }
      if((local_edge_sources[edge]&2U)!=0U) {
        open_hierarchy_edges.push_back(edge);
        ++result.unmatched_bcc_face_edges;
        if(result.unmatched_hierarchy_edge_geometry.size()<16U)
          result.unmatched_hierarchy_edge_geometry.push_back(geometry());
      }
    }
    const auto same_point=[&](std::uint32_t left,std::uint32_t right) {
      for(std::size_t axis=0U;axis<3U;++axis)
        if(std::abs(result.vertices[left][axis]-result.vertices[right][axis])>
           1.0e-9)return false;
      return true;
    };
    const auto point_on_segment=[&](std::uint32_t point,
                                    const std::array<std::uint32_t,2>& segment) {
      const auto make=[&](std::uint32_t index) {
        const auto& p=result.vertices[index];return Vec3{p[0],p[1],p[2]};
      };
      const auto p=make(point),a=make(segment[0]),b=make(segment[1]);
      const auto ab=b-a,ap=p-a;
      const double length_squared=dot(ab,ab);
      if(length_squared<=1.0e-24)return same_point(point,segment[0]);
      const double parameter=dot(ap,ab)/length_squared;
      if(parameter<-1.0e-9||parameter>1.0+1.0e-9)return false;
      const auto residual=ap-ab*parameter;
      return dot(residual,residual)<=1.0e-18*std::max(1.0,length_squared);
    };
    for(const auto surface_edge:open_surface_edges)
      if(std::ranges::any_of(open_hierarchy_edges,[&](const auto hierarchy_edge) {
           return (same_point(surface_edge[0],hierarchy_edge[0])&&
                   same_point(surface_edge[1],hierarchy_edge[1]))||
                  (same_point(surface_edge[0],hierarchy_edge[1])&&
                   same_point(surface_edge[1],hierarchy_edge[0]));
         }))
        ++result.geometrically_matching_open_edge_pairs;
    for(const auto surface_edge:open_surface_edges)
      if(std::ranges::any_of(open_hierarchy_edges,[&](const auto hierarchy_edge) {
           return point_on_segment(surface_edge[0],hierarchy_edge)&&
                  point_on_segment(surface_edge[1],hierarchy_edge);
         }))
        ++result.surface_edges_contained_in_hierarchy_edges;
    for(const auto surface_edge:open_surface_edges) {
      bool on_owner_face=false;
      for(std::size_t omitted=0U;omitted<4U;++omitted) {
        std::array<std::size_t,3> corners{};std::size_t cursor{};
        for(std::size_t i=0U;i<4U;++i)if(i!=omitted)corners[cursor++]=i;
        const auto a=owner_points[corners[0]];
        const auto normal=cross(owner_points[corners[1]]-a,
                                owner_points[corners[2]]-a);
        const double scale=std::max(1.0,std::sqrt(dot(normal,normal)));
        on_owner_face=on_owner_face||std::ranges::all_of(surface_edge,[&](std::uint32_t index) {
          const auto& p=result.vertices[index];
          return std::abs(dot(normal,Vec3{p[0],p[1],p[2]}-a))<=1.0e-8*scale;
        });
      }
      if(on_owner_face)++result.open_surface_edges_on_hierarchy_faces;
    }
    for(const auto hierarchy_edge:open_hierarchy_edges)
      if(std::ranges::any_of(open_surface_edges,[&](const auto surface_edge) {
           return point_on_segment(hierarchy_edge[0],surface_edge)&&
                  point_on_segment(hierarchy_edge[1],surface_edge);
         }))
        ++result.hierarchy_edges_contained_in_surface_edges;
    if(result.local_boundary_open_edges!=open_edges_before)
      result.locally_open_owners.push_back(owner);
    std::map<std::uint32_t,std::vector<std::uint32_t>> open_adjacency;
    for(const auto& [edge,count]:local_edge_uses)if(count!=2U) {
      open_adjacency[edge[0]].push_back(edge[1]);
      open_adjacency[edge[1]].push_back(edge[0]);
    }
    std::set<std::uint32_t> visited;
    for(const auto& [vertex,neighbours]:open_adjacency) {
      if(neighbours.size()!=2U)++result.open_boundary_bad_degree_vertices;
      if(visited.contains(vertex))continue;
      ++result.open_boundary_components;
      std::vector<std::uint32_t> pending{vertex},component;
      std::size_t component_vertices{};
      while(!pending.empty()) {
        const auto current=pending.back();pending.pop_back();
        if(!visited.insert(current).second)continue;
        ++component_vertices;
        component.push_back(current);
        for(const auto neighbour:open_adjacency[current])
          if(!visited.contains(neighbour))pending.push_back(neighbour);
      }
      result.maximum_open_boundary_vertices=std::max(
          result.maximum_open_boundary_vertices,component_vertices);
      const bool even=std::ranges::all_of(component,[&](std::uint32_t point) {
        return !open_adjacency[point].empty()&&open_adjacency[point].size()%2U==0U;
      });
      if(!even) { ++result.refused_closure_components;continue; }
      std::set<std::array<std::uint32_t,2>> remaining;
      for(const auto& [edge,count]:local_edge_uses)
        if(count!=2U&&std::ranges::find(component,edge[0])!=component.end()&&
           std::ranges::find(component,edge[1])!=component.end())remaining.insert(edge);
      bool component_closed=true;
      while(!remaining.empty()) {
        const auto start=remaining.begin()->at(0);
        std::vector<std::uint32_t> trail{start};
        auto current=start;
        do {
          auto selected=remaining.end();
          for(auto edge=remaining.begin();edge!=remaining.end();++edge)
            if((*edge)[0]==current||(*edge)[1]==current) { selected=edge;break; }
          if(selected==remaining.end()) { component_closed=false;break; }
          const auto edge=*selected;remaining.erase(selected);
          current=edge[0]==current?edge[1]:edge[0];trail.push_back(current);
        } while(current!=start&&trail.size()<=local_edge_uses.size()+1U);
        if(!component_closed||current!=start) { component_closed=false;break; }
        // Split a closed Euler trail at repeated vertices. Each resulting
        // simple cycle gets its own centre, so a degree-four touch does not
        // create a non-manifold radial edge.
        std::vector<std::uint32_t> stack;
        std::map<std::uint32_t,std::size_t> stack_position;
        for(const auto point:trail) {
          const auto repeated=stack_position.find(point);
          if(repeated==stack_position.end()) {
            stack_position.emplace(point,stack.size());stack.push_back(point);continue;
          }
          const auto begin=repeated->second;
          std::vector<std::uint32_t> ring(stack.begin()+static_cast<std::ptrdiff_t>(begin),
                                          stack.end());
          if(ring.size()<3U) { component_closed=false;break; }
          for(std::size_t i=begin+1U;i<stack.size();++i)stack_position.erase(stack[i]);
          stack.resize(begin+1U);
          const auto supporting=[&](std::array<std::uint32_t,3> face) {
            const auto make=[&](std::uint32_t index) {
              const auto& p=result.vertices[index];return Vec3{p[0],p[1],p[2]};
            };
            const auto a=make(face[0]),b=make(face[1]),c=make(face[2]);
            const auto normal=cross(b-a,c-a);
            const double scale=std::sqrt(dot(normal,normal));
            if(scale<=1.0e-12)return false;
            double low=std::numeric_limits<double>::infinity();
            double high=-std::numeric_limits<double>::infinity();
            for(const auto index:material_indices) {
              const double distance=dot(normal,make(index)-a);
              low=std::min(low,distance);high=std::max(high,distance);
            }
            return low>=-1.0e-9*scale||high<=1.0e-9*scale;
          };
          std::function<bool(std::size_t,std::size_t,
                             std::vector<std::array<std::uint32_t,3>>&)> triangulate;
          triangulate=[&](std::size_t first,std::size_t last,
                          std::vector<std::array<std::uint32_t,3>>& faces) {
            if(last<=first+1U)return true;
            for(std::size_t middle=first+1U;middle<last;++middle) {
              const std::array<std::uint32_t,3> face{{ring[first],ring[middle],ring[last]}};
              if(!supporting(face))continue;
              std::vector<std::array<std::uint32_t,3>> left,right;
              if(!triangulate(first,middle,left)||!triangulate(middle,last,right))continue;
              faces.insert(faces.end(),left.begin(),left.end());faces.push_back(face);
              faces.insert(faces.end(),right.begin(),right.end());return true;
            }
            return false;
          };
          std::vector<std::array<std::uint32_t,3>> closure_faces;
          if(!triangulate(0U,ring.size()-1U,closure_faces)||
             !std::ranges::all_of(closure_faces,[&](const auto& face) {
               return is_domain_boundary(face,result.vertices);
             })) {
            component_closed=false;break;
          }
          for(const auto face:closure_faces) {
            boundary.push_back(face);result.finite_closure_triangles.push_back(face);
          }
          result.closure_triangles+=closure_faces.size();
          ++result.closure_components;
        }
        if(!component_closed)break;
      }
      if(!component_closed||!remaining.empty())++result.refused_closure_components;
    }
    double owner_boundary_volume{};
    for(auto face:boundary) {
      std::array<std::uint32_t,4> tet{{centre,face[0],face[1],face[2]}};
      double volume=six_volume(tet);
      if(volume<0.0) { std::swap(tet[2],tet[3]);volume=-volume; }
      if(volume<=1.0e-13)result.positive=false;
      result.transition_tetrahedra.push_back(tet);
      result.transition_tetrahedron_owners.push_back(owner);
      result.tetrahedron_volume+=volume/6.0;
      const auto make=[&](std::uint32_t i) {
        const auto& p=result.vertices[i];return Vec3{p[0],p[1],p[2]};
      };
      auto pa=make(face[0]),pb=make(face[1]),pc=make(face[2]);
      const Vec3 inside{centroid[0],centroid[1],centroid[2]};
      if(dot(cross(pb-pa,pc-pa),inside-pa)>0.0)std::swap(pb,pc);
      owner_boundary_volume+=dot(pa,cross(pb,pc))/6.0;
      transition_boundary.push_back(face);
    }
    result.boundary_volume+=std::abs(owner_boundary_volume);
  }

  std::set<WorldTetAddress> cut(partition.cut_owners.begin(),partition.cut_owners.end());
  std::set<WorldTetAddress> retained(retained_owners.begin(),retained_owners.end());
  std::vector<WorldTetAddress> frontier;
  frontier.assign(hierarchy_roots.begin(),hierarchy_roots.end());
  for(unsigned int depth=0U;depth<red_depth;++depth) {
    std::vector<WorldTetAddress> children;
    for(const auto owner:frontier)for(std::uint8_t child=0U;child<8U;++child)
      children.push_back(owner.child(child));
    frontier.swap(children);
  }
  const auto append_full=[&](WorldTetAddress owner,auto& destination,auto& owners) {
    const auto keys=world_tetrahedron_vertex_keys(owner);
    std::array<std::uint32_t,4> tet{{hierarchy_index(keys[0]),hierarchy_index(keys[1]),
                                     hierarchy_index(keys[2]),hierarchy_index(keys[3])}};
    double volume=six_volume(tet);
    if(volume<0.0) { std::swap(tet[0],tet[1]);volume=-volume; }
    if(volume<=1.0e-13)result.positive=false;
    destination.push_back(tet);owners.push_back(owner);result.tetrahedron_volume+=volume/6.0;
    result.boundary_volume+=volume/6.0;
  };
  for(const auto owner:frontier) {
    if(cut.contains(owner)||retained.contains(owner))continue;
    const auto keys=world_tetrahedron_vertex_keys(owner);
    if(std::ranges::all_of(keys,[&](WorldVertexKey key) {
         const auto p=hierarchy_position(key);
         return field(p)<-plane_epsilon;
       })) {
      append_full(owner,result.transition_tetrahedra,
                  result.transition_tetrahedron_owners);++result.eroded_full_tetrahedra;
    }
  }
  result.cut_owner_tetrahedra=result.transition_tetrahedra.size()-
                               result.eroded_full_tetrahedra;
  for(const auto owner:retained_owners)
    append_full(owner,result.retained_core_tetrahedra,
                result.retained_core_tetrahedron_owners);

  using Face=std::array<std::uint32_t,3>;
  std::map<Face,std::size_t> face_uses;
  std::map<Face,std::vector<int>> face_sides;
  const auto count_faces=[&](const auto& tetrahedra) {
    for(const auto& tet:tetrahedra)for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet[i];
      std::sort(face.begin(),face.end());++face_uses[face];
      const auto make=[&](std::uint32_t index) {
        const auto& p=result.vertices[index];return Vec3{p[0],p[1],p[2]};
      };
      const auto a=make(face[0]),b=make(face[1]),c=make(face[2]);
      const double side=dot(cross(b-a,c-a),make(tet[omitted])-a);
      face_sides[face].push_back(side>0.0?1:-1);
    }
  };
  count_faces(result.transition_tetrahedra);count_faces(result.retained_core_tetrahedra);
  std::set<Face> exact_surface;
  for(auto face:result.exact_surface_triangles) {
    std::sort(face.begin(),face.end());exact_surface.insert(face);
  }
  std::set<Face> finite_closure;
  for(auto face:result.finite_closure_triangles) {
    std::sort(face.begin(),face.end());finite_closure.insert(face);
  }
  result.face_incidence_valid=true;
  for(const auto& [face,count]:face_uses) {
    if(count>2U) { result.face_incidence_valid=false;continue; }
    if(count==2U) {
      if(face_sides[face][0]==face_sides[face][1]) {
        ++result.same_sided_shared_faces;result.face_incidence_valid=false;
      }
      continue;
    }
    if(exact_surface.contains(face)||finite_closure.contains(face))continue;
    const bool root_boundary=is_domain_boundary(face,result.vertices);
    if(root_boundary)continue;
    if(is_chunk_interface(face,result.vertices)) {
      ++result.chunk_interface_faces;
      continue;
    }
    ++result.unpaired_non_domain_faces;result.face_incidence_valid=false;
  }
  result.exact_surface_preserved=true;
  for(const auto& face:exact_surface)
    if(face_uses[face]!=1U)result.exact_surface_preserved=false;
  result.volume_error=std::abs(result.tetrahedron_volume-result.boundary_volume);
  result.exact_volume=result.volume_error<=1.0e-9*std::max(1.0,result.boundary_volume);
  std::vector<std::array<std::uint32_t,4>> all_tetrahedra=result.transition_tetrahedra;
  all_tetrahedra.insert(all_tetrahedra.end(),result.retained_core_tetrahedra.begin(),
                        result.retained_core_tetrahedra.end());
  std::vector<WorldTetAddress> all_owners=result.transition_tetrahedron_owners;
  all_owners.insert(all_owners.end(),result.retained_core_tetrahedron_owners.begin(),
                    result.retained_core_tetrahedron_owners.end());
  struct Bounds { Vec3 low,high; };
  std::vector<Bounds> bounds;
  bounds.reserve(all_tetrahedra.size());
  for(const auto& tet:all_tetrahedra) {
    const auto& first=result.vertices[tet[0]];
    Bounds box{{first[0],first[1],first[2]},{first[0],first[1],first[2]}};
    for(std::size_t corner=1U;corner<4U;++corner)for(std::size_t axis=0U;axis<3U;++axis) {
      const double value=result.vertices[tet[corner]][axis];
      if(axis==0U) { box.low.x=std::min(box.low.x,value);box.high.x=std::max(box.high.x,value); }
      else if(axis==1U) { box.low.y=std::min(box.low.y,value);box.high.y=std::max(box.high.y,value); }
      else { box.low.z=std::min(box.low.z,value);box.high.z=std::max(box.high.z,value); }
    }
    bounds.push_back(box);
  }
  for(std::size_t first=0U;first<all_tetrahedra.size();++first)
    for(std::size_t second=first+1U;second<all_tetrahedra.size();++second) {
      const auto& a=bounds[first];const auto& b=bounds[second];
      if(a.high.x<=b.low.x+1.0e-12||b.high.x<=a.low.x+1.0e-12||
         a.high.y<=b.low.y+1.0e-12||b.high.y<=a.low.y+1.0e-12||
         a.high.z<=b.low.z+1.0e-12||b.high.z<=a.low.z+1.0e-12)continue;
      ++result.overlap_candidates;
      std::array<Vec3,4> first_points{},second_points{};
      for(std::size_t corner=0U;corner<4U;++corner) {
        const auto& p=result.vertices[all_tetrahedra[first][corner]];
        const auto& q=result.vertices[all_tetrahedra[second][corner]];
        first_points[corner]={p[0],p[1],p[2]};second_points[corner]={q[0],q[1],q[2]};
      }
      if(strict_tetrahedra_overlap(first_points,second_points)) {
        ++result.strict_overlap_pairs;
        if(all_owners[first]==all_owners[second])++result.same_owner_overlap_pairs;
        else ++result.cross_owner_overlap_pairs;
      }
      }
  constexpr double radians_to_degrees=57.2957795130823208768;
  for(const auto& tet:all_tetrahedra) {
    std::array<Vec3,4> points{};
    for(std::size_t i=0U;i<4U;++i) {
      const auto& p=result.vertices[tet[i]];points[i]={p[0],p[1],p[2]};
    }
    for(std::size_t first=0U;first<4U;++first)
      for(std::size_t second=first+1U;second<4U;++second) {
        std::size_t third=4U,fourth=4U;
        for(std::size_t i=0U;i<4U;++i)if(i!=first&&i!=second) {
          if(third==4U)third=i;else fourth=i;
        }
        const auto a=cross(points[second]-points[first],points[third]-points[first]);
        const auto b=cross(points[first]-points[second],points[fourth]-points[second]);
        const double denominator=std::sqrt(dot(a,a)*dot(b,b));
        if(denominator<=1.0e-24)continue;
        const double angle=std::acos(std::clamp(dot(a,b)/denominator,-1.0,1.0))*
                           radians_to_degrees;
        result.minimum_dihedral_degrees=std::min(result.minimum_dihedral_degrees,angle);
        result.maximum_dihedral_degrees=std::max(result.maximum_dihedral_degrees,angle);
      }
  }
  result.retained_bytes=result.vertices.size()*sizeof(result.vertices.front())+
      all_tetrahedra.size()*sizeof(all_tetrahedra.front())+
      all_owners.size()*sizeof(all_owners.front())+
      (result.exact_surface_triangles.size()+result.finite_closure_triangles.size())*
          sizeof(std::array<std::uint32_t,3>);
  result.no_overlap_by_scaffold_partition=result.strict_overlap_pairs==0U;
  result.valid=result.convex_route_applicable&&result.positive&&
      result.exact_surface_preserved&&result.face_incidence_valid&&result.exact_volume&&
      result.no_overlap_by_scaffold_partition;
  return result;
}
} // namespace

BccScaffoldTransitionVolume construct_bcc_scaffold_convex_transition(
    const SandwichConfig& config,
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core) {
  const auto convexity=inspect_bcc_scaffold_convex_material_cells(config,surface,core);
  const auto partition=partition_bcc_surface_over_transition_scaffold(surface,core);
  std::array<WorldTetAddress,bcc_root_tetrahedron_count> roots{};
  for(std::uint8_t root=0U;root<bcc_root_tetrahedron_count;++root)
    roots[root]=WorldTetAddress::root(root);
  const auto position=[](WorldVertexKey key) {return probe_position(key);};
  const auto field=[&](Vec3 point) {
    return evaluate_sandwich_field(config,{point.x,point.y,point.z});
  };
  const auto boundary=[](const std::array<std::uint32_t,3>& face,
                         const std::vector<std::array<double,3>>& vertices) {
    for(std::size_t axis=0U;axis<3U;++axis) {
      bool low=true,high=true;
      for(const auto index:face) {
        low=low&&std::abs(vertices[index][axis]+1.0)<=1.0e-9;
        high=high&&std::abs(vertices[index][axis]-1.0)<=1.0e-9;
      }
      if(low||high)return true;
    }
    return false;
  };
  const auto no_chunk_interface=[](const std::array<std::uint32_t,3>&,
                                   const std::vector<std::array<double,3>>&) {
    return false;
  };
  return construct_scaffold_convex_transition(
      partition,surface.red_depth,core.logical_owners,roots,position,field,
      boundary,no_chunk_interface,convexity.complete_convex_route);
}

namespace {
BccScaffoldTransitionVolume construct_structured_global_cut_transition_impl(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface,
    bool interior_only) {
  const auto convexity=inspect_structured_global_cut_cells(config,surface);
  auto partition=partition_structured_surface_over_global_core(surface);
  const auto hierarchy_root=world_tetrahedron_geometry(
      WorldTetAddress::root(0U));
  const auto root_owner=[&](WorldTetAddress owner) {
    ::tetra::Vec3 centroid{};
    for(const auto point:world_tetrahedron_geometry(owner))
      centroid=centroid+point;
    centroid=centroid/4.0;
    const auto subtract=[](::tetra::Vec3 a,::tetra::Vec3 b) {
      return ::tetra::Vec3{a.x-b.x,a.y-b.y,a.z-b.z};
    };
    const auto cross=[](::tetra::Vec3 a,::tetra::Vec3 b) {
      return ::tetra::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,
                           a.x*b.y-a.y*b.x};
    };
    const auto dot=[](::tetra::Vec3 a,::tetra::Vec3 b) {
      return a.x*b.x+a.y*b.y+a.z*b.z;
    };
    const auto a=subtract(hierarchy_root[1],hierarchy_root[0]);
    const auto b=subtract(hierarchy_root[2],hierarchy_root[0]);
    const auto c=subtract(hierarchy_root[3],hierarchy_root[0]);
    const auto d=subtract(centroid,hierarchy_root[0]);
    const double determinant=dot(a,cross(b,c));
    const double u=dot(d,cross(b,c))/determinant;
    const double v=dot(a,cross(d,c))/determinant;
    const double w=dot(a,cross(b,d))/determinant;
    const std::array<double,4> weights{{1.0-u-v-w,u,v,w}};
    return static_cast<std::uint8_t>(std::distance(
        weights.begin(),std::max_element(weights.begin(),weights.end())));
  };
  // Ghost surface is read-only neighborhood input. Only the two requested
  // hexahedral regions own output; halo-only cut tets must not be emitted.
  std::erase_if(partition.triangles,[&](const auto& triangle) {
    return root_owner(triangle.owner)>=2U;
  });
  std::erase_if(partition.cut_owners,[&](WorldTetAddress owner) {
    return root_owner(owner)>=2U;
  });
  std::erase_if(partition.source_boundary_owners,[&](WorldTetAddress owner) {
    return root_owner(owner)>=2U;
  });
  if(interior_only) {
    const std::set<WorldTetAddress> direct_boundary(
        partition.source_boundary_owners.begin(),
        partition.source_boundary_owners.end());
    // The finite sheet can terminate through a neighboring tet while this
    // tet owns only the endpoint of that termination. Remove the complete
    // two-face hierarchy ring around the artificial source boundary; this
    // diagnostic is meant to isolate cells whose entire surface neighborhood
    // is present, not to pretend that a truncated neighbor supplied a facet.
    const auto owner_faces=[](WorldTetAddress owner) {
      const auto keys=world_tetrahedron_vertex_keys(owner);
      std::array<std::array<WorldVertexKey,3>,4> result{};
      for(std::size_t omitted=0U;omitted<4U;++omitted) {
        std::size_t cursor{};
        for(std::size_t corner=0U;corner<4U;++corner)
          if(corner!=omitted)result[omitted][cursor++]=keys[corner];
        std::ranges::sort(result[omitted]);
      }
      return result;
    };
    std::set<WorldTetAddress> excluded=direct_boundary;
    for(unsigned int ring=0U;ring<1U;++ring) {
      std::set<std::array<WorldVertexKey,3>> boundary_faces;
      for(const auto owner:excluded) {
        const auto faces=owner_faces(owner);
        boundary_faces.insert(faces.begin(),faces.end());
      }
      auto expanded=excluded;
      for(const auto owner:partition.cut_owners) {
        const auto faces=owner_faces(owner);
        if(std::ranges::any_of(faces,[&](const auto& face) {
             return boundary_faces.contains(face);
           }))
          expanded.insert(owner);
      }
      excluded=std::move(expanded);
    }
    std::erase_if(partition.triangles,[&](const auto& triangle) {
      return excluded.contains(triangle.owner);
    });
    std::erase_if(partition.cut_owners,[&](WorldTetAddress owner) {
      return excluded.contains(owner);
    });
  }
  const auto root=WorldTetAddress::root(0U);
  const auto reference=world_tetrahedron_geometry(root);
  std::array<Vec3,4> target{};
  for(std::size_t corner=0U;corner<4U;++corner) {
    const auto& p=surface.parent_tetrahedron[corner];
    target[corner]={p[0],p[1],p[2]};
  }
  const auto cross=[](Vec3 a,Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto transform=[&](::tetra::Vec3 point) {
    const Vec3 origin{reference[0].x,reference[0].y,reference[0].z};
    const Vec3 a{reference[1].x-origin.x,reference[1].y-origin.y,
                 reference[1].z-origin.z};
    const Vec3 b{reference[2].x-origin.x,reference[2].y-origin.y,
                 reference[2].z-origin.z};
    const Vec3 c{reference[3].x-origin.x,reference[3].y-origin.y,
                 reference[3].z-origin.z};
    const Vec3 d{point.x-origin.x,point.y-origin.y,point.z-origin.z};
    const double determinant=dot(a,cross(b,c));
    const double u=dot(d,cross(b,c))/determinant;
    const double v=dot(a,cross(d,c))/determinant;
    const double w=dot(a,cross(b,d))/determinant;
    const std::array<double,4> weights{{1.0-u-v-w,u,v,w}};
    Vec3 mapped{};
    for(std::size_t corner=0U;corner<4U;++corner)
      mapped=mapped+target[corner]*weights[corner];
    return mapped;
  };
  const auto position=[&](WorldVertexKey key) {
    // `world_tetrahedron_geometry()` and `reference` use the hierarchy's
    // native [0,1] cube. `probe_position()` is deliberately normalized to
    // [-1,1] for the older BCC probe, so using it here applies the parent
    // affine transform in the wrong frame. Reconstruct the native coordinate
    // directly from the stable key instead.
    const double denominator=std::ldexp(1.0,key.denominator_exponent);
    return transform({static_cast<double>(key.x)/denominator,
                      static_cast<double>(key.y)/denominator,
                      static_cast<double>(key.z)/denominator});
  };
  const auto field=[&](Vec3 point) {
    return evaluate_sandwich_field(config,{point.x,point.y,point.z});
  };
  const auto boundary=[&,target](const std::array<std::uint32_t,3>& face,
                                 const std::vector<std::array<double,3>>& vertices) {
    constexpr std::array<std::array<std::size_t,3>,4> faces{{
        {{1U,2U,3U}},{{0U,3U,2U}},{{0U,1U,3U}},{{0U,2U,1U}}}};
    for(const auto corners:faces) {
      const auto normal=cross(target[corners[1]]-target[corners[0]],
                              target[corners[2]]-target[corners[0]]);
      const double scale=std::max(1.0,std::sqrt(dot(normal,normal)));
      bool coplanar=true;
      for(const auto index:face) {
        const auto& p=vertices[index];
        coplanar=coplanar&&std::abs(dot(normal,
            Vec3{p[0],p[1],p[2]}-target[corners[0]]))<=1.0e-9*scale;
      }
      if(coplanar)return true;
    }
    return false;
  };
  // A chunk boundary is a face of the unchanged hierarchy, never a plane cut
  // through a tet. Enumerate the uniform root-zero leaf adjacency and retain
  // only faces whose incident whole-tet owners fall on opposite sides of the
  // selected (regions 0/1) emission set.
  std::vector<WorldTetAddress> leaves{root};
  for(unsigned int depth=0U;depth<surface.global_core_red_depth;++depth) {
    std::vector<WorldTetAddress> children;
    children.reserve(leaves.size()*8U);
    for(const auto owner:leaves)for(std::uint8_t child=0U;child<8U;++child)
      children.push_back(owner.child(child));
    leaves.swap(children);
  }
  std::map<std::array<WorldVertexKey,3>,std::vector<WorldTetAddress>> face_owners;
  for(const auto owner:leaves) {
    const auto keys=world_tetrahedron_vertex_keys(owner);
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      std::array<WorldVertexKey,3> face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=keys[corner];
      std::ranges::sort(face);face_owners[face].push_back(owner);
    }
  }
  std::vector<std::array<Vec3,3>> chunk_interface_triangles;
  for(const auto& [face,owners]:face_owners) {
    if(owners.size()!=2U)continue;
    const bool first_selected=root_owner(owners[0])<2U;
    const bool second_selected=root_owner(owners[1])<2U;
    if(first_selected==second_selected)continue;
    chunk_interface_triangles.push_back(
        {{position(face[0]),position(face[1]),position(face[2])}});
  }
  const auto chunk_interface=[&,chunk_interface_triangles](
      const std::array<std::uint32_t,3>& face,
      const std::vector<std::array<double,3>>& vertices) {
    constexpr double epsilon=1.0e-8;
    for(const auto& support:chunk_interface_triangles) {
      const auto normal=cross(support[1]-support[0],support[2]-support[0]);
      const double normal2=dot(normal,normal);
      if(normal2<=1.0e-24)continue;
      bool contained=true;
      for(const auto index:face) {
        const auto& raw=vertices[index];const Vec3 point{raw[0],raw[1],raw[2]};
        if(std::abs(dot(normal,point-support[0]))>
           epsilon*std::sqrt(normal2)) { contained=false;break; }
        const auto v0=support[1]-support[0];
        const auto v1=support[2]-support[0];
        const auto v2=point-support[0];
        const double d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1);
        const double d20=dot(v2,v0),d21=dot(v2,v1);
        const double denominator=d00*d11-d01*d01;
        if(std::abs(denominator)<=1.0e-24) { contained=false;break; }
        const double v=(d11*d20-d01*d21)/denominator;
        const double w=(d00*d21-d01*d20)/denominator;
        const double u=1.0-v-w;
        if(u<-epsilon||v<-epsilon||w<-epsilon) { contained=false;break; }
      }
      if(contained)return true;
    }
    return false;
  };
  // The structured fixture owns only the two selected hexahedral query
  // regions. Do not sweep the rest of root zero for extra full tets here:
  // doing so would silently turn a two-chunk request into a four-chunk core.
  // The retained list is the independently owned whole-tet set; cut owners
  // are supplied by the exact DC partition above.
  const std::array<WorldTetAddress,0> roots{};
  return construct_scaffold_convex_transition(
      partition,surface.global_core_red_depth,surface.global_core_tet_addresses,
      roots,position,field,boundary,chunk_interface,
      convexity.complete_convex_route);
}
} // namespace

BccScaffoldTransitionVolume construct_structured_global_cut_transition(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface) {
  return construct_structured_global_cut_transition_impl(config,surface,false);
}

BccScaffoldTransitionVolume construct_structured_global_interior_cut_transition(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface) {
  return construct_structured_global_cut_transition_impl(config,surface,true);
}

const char* bcc_transition_request_failure_name(
    BccTransitionRequestFailure failure) {
  switch(failure) {
    case BccTransitionRequestFailure::none:return "none";
    case BccTransitionRequestFailure::invalid_surface:return "invalid_surface";
    case BccTransitionRequestFailure::incompatible_hierarchy:return "incompatible_hierarchy";
    case BccTransitionRequestFailure::unsupported_address_depth:return "unsupported_address_depth";
    case BccTransitionRequestFailure::malformed_interface:return "malformed_interface";
    case BccTransitionRequestFailure::stable_id_collision:return "stable_id_collision";
    case BccTransitionRequestFailure::invalid_closed_contract:return "invalid_closed_contract";
  }
  return "unknown";
}

BccTransitionOwnerPartition inspect_bcc_transition_owner_partition(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core) {
  BccTransitionOwnerPartition result;
  if(surface.red_depth!=core.red_depth||surface.triangle_owners.size()!=
       surface.triangles.size()||core.interface_face_owners.size()!=
       core.interface_faces.size()||surface.red_depth==0U)return result;
  std::map<WorldTetAddress,std::size_t> surface_leaves,interface_leaves;
  std::map<WorldTetAddress,std::size_t> surface_parents,interface_parents;
  for(const auto owner:surface.triangle_owners) {
    ++surface_leaves[owner];
    ++surface_parents[owner.parent()];
  }
  for(const auto owner:core.interface_face_owners) {
    ++interface_leaves[owner];
    ++interface_parents[owner.parent()];
  }
  result.surface_owners=surface_leaves.size();
  result.interface_owners=interface_leaves.size();
  result.parent_surface_owners=surface_parents.size();
  result.parent_interface_owners=interface_parents.size();
  for(const auto& [owner,count]:surface_leaves) {
    (void)count;
    result.exact_shared_owners+=interface_leaves.contains(owner)?1U:0U;
  }
  for(const auto& [owner,count]:surface_parents) {
    result.maximum_surface_faces_per_parent=std::max(
        result.maximum_surface_faces_per_parent,count);
    result.shared_parent_owners+=interface_parents.contains(owner)?1U:0U;
  }
  for(const auto& [owner,count]:interface_parents) {
    (void)owner;
    result.maximum_interface_faces_per_parent=std::max(
        result.maximum_interface_faces_per_parent,count);
  }
  result.deterministic_bounded_groups=!surface_parents.empty()&&
      !interface_parents.empty()&&result.maximum_surface_faces_per_parent<=64U&&
      result.maximum_interface_faces_per_parent<=32U;
  return result;
}

BccTransitionParentPartition partition_bcc_transition_stars(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core,
    unsigned int grouping_depth) {
  BccTransitionParentPartition result;
  if(surface.red_depth!=core.red_depth||surface.red_depth==0U||
     grouping_depth>=surface.red_depth||
     surface.triangle_owners.size()!=surface.triangles.size()||
     core.interface_face_owners.size()!=core.interface_faces.size())return result;
  std::map<WorldTetAddress,BccTransitionParentPatch> patches;
  for(std::size_t index=0U;index<surface.triangles.size();++index) {
    const auto parent=surface.triangle_owners[index].ancestor(grouping_depth);
    auto& patch=patches[parent];
    patch.parent=parent;
    patch.surface_triangles.push_back(static_cast<std::uint32_t>(index));
  }
  for(std::size_t index=0U;index<core.interface_faces.size();++index) {
    const auto parent=core.interface_face_owners[index].ancestor(grouping_depth);
    auto& patch=patches[parent];
    patch.parent=parent;
    patch.interface_faces.push_back(static_cast<std::uint32_t>(index));
  }
  std::map<std::array<std::uint64_t,2>,std::size_t> surface_seam_uses;
  std::map<std::array<WorldVertexKey,2>,std::size_t> interface_seam_uses;
  const auto boundary_topology=[](const auto& edges) {
    using EdgeType=typename std::decay_t<decltype(edges)>::value_type;
    using VertexType=typename EdgeType::value_type;
    std::map<VertexType,std::set<VertexType>> adjacency;
    for(const auto& edge:edges) {
      adjacency[edge[0]].insert(edge[1]);
      adjacency[edge[1]].insert(edge[0]);
    }
    std::set<VertexType> visited;
    std::size_t components{};
    bool cycles=!adjacency.empty();
    for(const auto& [vertex,neighbours]:adjacency) {
      cycles=cycles&&neighbours.size()==2U;
      if(visited.contains(vertex))continue;
      ++components;
      std::vector<VertexType> pending{vertex};
      while(!pending.empty()) {
        const auto current=pending.back();pending.pop_back();
        if(!visited.insert(current).second)continue;
        for(const auto& next:adjacency.at(current))pending.push_back(next);
      }
    }
    return std::pair{components,cycles};
  };
  for(auto& [unused,patch]:patches) {
    (void)unused;
    std::map<std::array<std::uint64_t,2>,unsigned int> surface_edges;
    for(const auto index:patch.surface_triangles) {
      const auto triangle=surface.triangles[index];
      for(std::size_t edge=0U;edge<3U;++edge) {
        std::array<std::uint64_t,2> key{{
            surface_id(surface.vertex_owners[triangle[edge]]),
            surface_id(surface.vertex_owners[triangle[(edge+1U)%3U]])}};
        std::sort(key.begin(),key.end());
        ++surface_edges[key];
      }
    }
    for(const auto& [edge,count]:surface_edges)if(count==1U) {
      patch.surface_boundary_edges.push_back(edge);
      ++surface_seam_uses[edge];
    }
    std::tie(patch.surface_boundary_components,
             patch.surface_boundary_is_cycles)=
        boundary_topology(patch.surface_boundary_edges);
    std::map<std::array<WorldVertexKey,2>,unsigned int> interface_edges;
    for(const auto index:patch.interface_faces) {
      const auto face=core.interface_faces[index];
      for(std::size_t edge=0U;edge<3U;++edge) {
        auto key=std::array<WorldVertexKey,2>{{face[edge],face[(edge+1U)%3U]}};
        std::sort(key.begin(),key.end());
        ++interface_edges[key];
      }
    }
    for(const auto& [edge,count]:interface_edges)if(count==1U) {
      patch.interface_boundary_edges.push_back(edge);
      ++interface_seam_uses[edge];
    }
    std::tie(patch.interface_boundary_components,
             patch.interface_boundary_is_cycles)=
        boundary_topology(patch.interface_boundary_edges);
    if(!patch.surface_triangles.empty()&&!patch.interface_faces.empty()) {
      ++result.two_front_patches;
      if(patch.surface_boundary_components==1U&&
         patch.interface_boundary_components==1U&&
         patch.surface_boundary_is_cycles&&patch.interface_boundary_is_cycles)
        ++result.single_loop_two_front_patches;
    }
    result.surface_triangles+=patch.surface_triangles.size();
    result.interface_faces+=patch.interface_faces.size();
    result.patches.push_back(std::move(patch));
  }
  result.exact_partition=result.surface_triangles==surface.triangles.size()&&
      result.interface_faces==core.interface_faces.size();
  result.canonical_shared_seams=true;
  for(const auto& [unused,count]:surface_seam_uses) {
    (void)unused;
    if(count==2U)++result.shared_surface_seams;
    else if(count>2U)result.canonical_shared_seams=false;
  }
  for(const auto& [unused,count]:interface_seam_uses) {
    (void)unused;
    if(count==2U)++result.shared_interface_seams;
    else if(count>2U)result.canonical_shared_seams=false;
  }
  result.canonical_shared_seams=result.canonical_shared_seams&&
      result.shared_surface_seams>0U&&result.shared_interface_seams>0U;
  return result;
}

BccTransitionParentPartition partition_bcc_transition_parent_stars(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core) {
  if(surface.red_depth==0U)return {};
  return partition_bcc_transition_stars(surface,core,surface.red_depth-1U);
}

BccParentStarConeProbe probe_bcc_transition_star_cones(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core,
    unsigned int grouping_depth) {
  BccParentStarConeProbe result;
  const auto partition=partition_bcc_transition_stars(surface,core,grouping_depth);
  result.two_front_patches=partition.two_front_patches;
  const auto ordered_cycle=[](const auto& edges) {
    using EdgeType=typename std::decay_t<decltype(edges)>::value_type;
    using VertexType=typename EdgeType::value_type;
    std::map<VertexType,std::vector<VertexType>> adjacency;
    for(const auto& edge:edges) {
      adjacency[edge[0]].push_back(edge[1]);
      adjacency[edge[1]].push_back(edge[0]);
    }
    std::vector<VertexType> loop;
    if(adjacency.empty()||std::ranges::any_of(adjacency,[](const auto& item) {
         return item.second.size()!=2U;
       }))return loop;
    const auto first=adjacency.begin()->first;
    auto previous=first,current=*std::min_element(adjacency.at(first).begin(),
                                                  adjacency.at(first).end());
    loop.push_back(first);
    while(current!=first) {
      if(loop.size()>=adjacency.size())return std::vector<VertexType>{};
      loop.push_back(current);
      const auto& neighbours=adjacency.at(current);
      const auto next=neighbours[0]==previous?neighbours[1]:neighbours[0];
      previous=current;current=next;
    }
    if(loop.size()!=adjacency.size())return std::vector<VertexType>{};
    return loop;
  };
  struct Point { double x{},y{},z{}; };
  const auto subtract=[](Point a,Point b){return Point{a.x-b.x,a.y-b.y,a.z-b.z};};
  const auto cross=[](Point a,Point b){return Point{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto dot=[](Point a,Point b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto stable_edge=[](std::uint64_t a,std::uint64_t b) {
    if(b<a)std::swap(a,b);
    return std::array<std::uint64_t,2>{{a,b}};
  };
  for(const auto& patch:partition.patches) {
    if(patch.surface_triangles.empty()||patch.interface_faces.empty()||
       patch.surface_boundary_components!=1U||
       patch.interface_boundary_components!=1U||
       !patch.surface_boundary_is_cycles||!patch.interface_boundary_is_cycles)
      continue;
    ++result.single_loop_patches;
    std::map<std::uint64_t,Point> positions;
    const auto surface_loop_ids=ordered_cycle(patch.surface_boundary_edges);
    const auto interface_loop_keys=ordered_cycle(patch.interface_boundary_edges);
    if(surface_loop_ids.empty()||interface_loop_keys.empty())continue;
    std::vector<SharedLatticeLoopVertex> surface_loop,interface_loop;
    for(const auto stable:surface_loop_ids) {
      const auto found=std::find_if(surface.vertex_owners.begin(),surface.vertex_owners.end(),
          [&](BccHexCellAddress owner){return surface_id(owner)==stable;});
      if(found==surface.vertex_owners.end()){surface_loop.clear();break;}
      const auto index=static_cast<std::size_t>(found-surface.vertex_owners.begin());
      const auto& p=surface.vertices[index];
      surface_loop.push_back({stable,p});
      positions.emplace(stable,Point{p[0],p[1],p[2]});
    }
    for(const auto key:interface_loop_keys) {
      const auto stable=hierarchy_id(key);
      const auto p=probe_position(key);
      interface_loop.push_back({stable,{p.x,p.y,p.z}});
      positions.emplace(stable,Point{p.x,p.y,p.z});
    }
    if(surface_loop.empty()||interface_loop.empty())continue;
    const auto side=construct_shared_lattice_side_wall(surface_loop,interface_loop,256U);
    if(!side.accepted())continue;
    ++result.side_walls_accepted;
    std::vector<std::array<std::uint64_t,3>> faces;
    for(const auto index:patch.surface_triangles) {
      const auto triangle=surface.triangles[index];
      std::array<std::uint64_t,3> face{};
      for(std::size_t corner=0U;corner<3U;++corner) {
        face[corner]=surface_id(surface.vertex_owners[triangle[corner]]);
        const auto& p=surface.vertices[triangle[corner]];
        positions.emplace(face[corner],Point{p[0],p[1],p[2]});
      }
      faces.push_back(face);
    }
    for(const auto index:patch.interface_faces) {
      std::array<std::uint64_t,3> face{};
      for(std::size_t corner=0U;corner<3U;++corner) {
        face[corner]=hierarchy_id(core.interface_faces[index][corner]);
        const auto p=probe_position(core.interface_faces[index][corner]);
        positions.emplace(face[corner],Point{p.x,p.y,p.z});
      }
      faces.push_back(face);
    }
    faces.insert(faces.end(),side.triangles.begin(),side.triangles.end());
    result.maximum_boundary_faces=std::max(result.maximum_boundary_faces,faces.size());
    std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edge_uses;
    for(std::size_t face=0U;face<faces.size();++face)
      for(std::size_t edge=0U;edge<3U;++edge)
        edge_uses[stable_edge(faces[face][edge],faces[face][(edge+1U)%3U])].push_back(face);
    if(std::ranges::any_of(edge_uses,[](const auto& item){return item.second.size()!=2U;}))
      continue;
    std::vector<bool> oriented(faces.size());
    oriented.front()=true;
    std::vector<std::size_t> pending{0U};
    const auto follows=[&](std::size_t face,std::uint64_t a,std::uint64_t b) {
      for(std::size_t edge=0U;edge<3U;++edge)
        if(faces[face][edge]==a&&faces[face][(edge+1U)%3U]==b)return true;
      return false;
    };
    while(!pending.empty()) {
      const auto current=pending.back();pending.pop_back();
      const auto face=faces[current];
      for(std::size_t edge=0U;edge<3U;++edge) {
        const auto a=face[edge],b=face[(edge+1U)%3U];
        const auto& uses=edge_uses.at(stable_edge(a,b));
        const auto other=uses[0]==current?uses[1]:uses[0];
        if(oriented[other])continue;
        if(follows(other,a,b))std::swap(faces[other][1],faces[other][2]);
        oriented[other]=true;pending.push_back(other);
      }
    }
    if(std::ranges::any_of(oriented,[](bool value){return !value;}))continue;
    ++result.closed_boundaries;
    double signed_volume{};
    for(const auto face:faces) {
      const auto a=positions.at(face[0]),b=positions.at(face[1]),c=positions.at(face[2]);
      signed_volume+=dot(a,cross(b,c))/6.0;
    }
    if(signed_volume<0.0)
      for(auto& face:faces)std::swap(face[1],face[2]);
    BccParentStarConeProbe::ClosedBoundary closed;
    closed.owner=patch.parent;
    closed.faces=faces;
    for(const auto& [id,p]:positions)
      closed.vertices.push_back({id,{p.x,p.y,p.z}});
    Point kernel{};
    for(const auto& [unused,p]:positions) {
      (void)unused;kernel.x+=p.x;kernel.y+=p.y;kernel.z+=p.z;
    }
    const auto inverse=1.0/static_cast<double>(positions.size());
    kernel.x*=inverse;kernel.y*=inverse;kernel.z*=inverse;
    constexpr double margin=1.0e-10;
    for(std::size_t pass=0U;pass<512U;++pass) {
      bool changed=false;
      for(const auto face:faces) {
        const auto a=positions.at(face[0]),b=positions.at(face[1]),c=positions.at(face[2]);
        const auto normal=cross(subtract(b,a),subtract(c,a));
        const auto length_squared=dot(normal,normal);
        const auto violation=dot(normal,subtract(kernel,a))+margin*std::sqrt(length_squared);
        if(violation<=0.0)continue;
        kernel.x-=normal.x*violation/length_squared;
        kernel.y-=normal.y*violation/length_squared;
        kernel.z-=normal.z*violation/length_squared;
        changed=true;
      }
      if(!changed)break;
    }
    bool feasible=true;
    for(const auto face:faces) {
      const auto a=positions.at(face[0]),b=positions.at(face[1]),c=positions.at(face[2]);
      const auto normal=cross(subtract(b,a),subtract(c,a));
      feasible=feasible&&dot(normal,subtract(kernel,a))<
          -0.5*margin*std::sqrt(dot(normal,normal));
    }
    if(!feasible) {
      result.closed_patch_boundaries.push_back(std::move(closed));
      continue;
    }
    ++result.star_shaped_boundaries;
    result.candidate_tetrahedra+=faces.size();
    result.successful_parents.push_back(patch.parent);
    closed.star_shaped=true;
    result.closed_patch_boundaries.push_back(std::move(closed));
  }
  return result;
}

BccParentStarConeProbe probe_bcc_parent_star_cones(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core) {
  if(surface.red_depth==0U)return {};
  return probe_bcc_transition_star_cones(surface,core,surface.red_depth-1U);
}

BccSurfaceCoreTransitionRequest make_bcc_surface_core_transition_request(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core,
    double skirt_expansion,double cap_clearance) {
  BccSurfaceCoreTransitionRequest result;
  const auto refuse=[&](BccTransitionRequestFailure failure) {
    result.failure=failure;
    return result;
  };
  if(!surface.validation.valid||surface.vertices.empty()||
     surface.vertices.size()!=surface.vertex_owners.size()||
     surface.triangles.empty()||!std::isfinite(skirt_expansion)||
     !std::isfinite(cap_clearance)||skirt_expansion<=0.0||cap_clearance<=0.0)
    return refuse(BccTransitionRequestFailure::invalid_surface);
  if(surface.red_depth!=core.red_depth||core.logical_owners.empty()||
     core.interface_faces.empty()||
     core.interface_faces.size()!=core.interface_face_owners.size())
    return refuse(BccTransitionRequestFailure::incompatible_hierarchy);
  if(surface.red_depth>16U)
    return refuse(BccTransitionRequestFailure::unsupported_address_depth);

  auto& input=result.input;
  input.coordinate_scale=4.0;
  // The exact DC sheet is allowed to contain poor but nondegenerate input
  // triangles.  Element quality is assessed after recovery, not by silently
  // deleting immutable PLC facets here.
  input.minimum_outer_triangle_angle_degrees=0.0;
  std::map<std::uint64_t,std::uint32_t> indexes;
  const auto add=[&](std::uint64_t id,Vec3 point) -> std::uint32_t {
    if(const auto found=indexes.find(id);found!=indexes.end()) {
      const auto& previous=input.vertices[found->second];
      if(previous.x!=point.x||previous.y!=point.y||previous.z!=point.z)
        throw std::logic_error("stable BCC transition id changed geometry");
      return found->second;
    }
    const auto index=static_cast<std::uint32_t>(input.vertices.size());
    indexes.emplace(id,index);
    input.stable_vertex_ids.push_back(id);
    input.vertices.push_back(point);
    return index;
  };

  try {
    std::vector<std::uint32_t> top(surface.vertices.size());
    Vec3 centre{};
    for(std::size_t i=0U;i<surface.vertices.size();++i) {
      const auto& p=surface.vertices[i];
      top[i]=add(bcc_transition_surface_vertex_id(surface.vertex_owners[i]),{p[0],p[1],p[2]});
      centre.x+=p[0];centre.y+=p[1];centre.z+=p[2];
    }
    const auto inverse=1.0/static_cast<double>(surface.vertices.size());
    centre.x*=inverse;centre.y*=inverse;centre.z*=inverse;

    std::map<std::array<std::uint32_t,2>,std::vector<std::array<std::uint32_t,2>>> edge_uses;
    for(const auto triangle:surface.triangles) {
      if(std::ranges::any_of(triangle,[&](auto vertex){return vertex>=top.size();}))
        return refuse(BccTransitionRequestFailure::invalid_surface);
      input.outer_faces.push_back({top[triangle[0]],top[triangle[1]],top[triangle[2]]});
      for(std::size_t edge=0U;edge<3U;++edge) {
        const std::array<std::uint32_t,2> directed{{triangle[edge],triangle[(edge+1U)%3U]}};
        auto key=directed;
        std::ranges::sort(key);
        edge_uses[key].push_back(directed);
      }
    }
    result.frozen_surface_faces=surface.triangles.size();

    double core_minimum_z=std::numeric_limits<double>::infinity();
    for(const auto owner:core.logical_owners)
      for(const auto key:world_tetrahedron_vertex_keys(owner))
        core_minimum_z=std::min(core_minimum_z,probe_position(key).z);
    const double collar_z=core_minimum_z-cap_clearance;
    const double cap_z=collar_z-cap_clearance;
    std::map<std::uint32_t,std::uint32_t> cap;
    std::map<std::uint32_t,std::uint32_t> collar;
    std::vector<std::array<std::uint32_t,2>> boundary;
    for(const auto& [unused,uses]:edge_uses) {
      (void)unused;
      if(uses.size()==2U)continue;
      if(uses.size()!=1U)return refuse(BccTransitionRequestFailure::invalid_surface);
      const auto a=uses.front()[0],b=uses.front()[1];
      boundary.push_back({a,b});
      for(const auto vertex:{a,b})if(!cap.contains(vertex)) {
        const auto& p=surface.vertices[vertex];
        collar.emplace(vertex,add(collar_id(surface.vertex_owners[vertex]),
                                   {p[0],p[1],collar_z}));
        const Vec3 lower{centre.x+(p[0]-centre.x)*(1.0+skirt_expansion),
                         centre.y+(p[1]-centre.y)*(1.0+skirt_expansion),cap_z};
        cap.emplace(vertex,add(cap_id(surface.vertex_owners[vertex]),lower));
      }
      input.outer_faces.push_back({top[b],top[a],collar.at(a)});
      input.outer_faces.push_back({top[b],collar.at(a),collar.at(b)});
      input.outer_faces.push_back({collar.at(b),collar.at(a),cap.at(a)});
      input.outer_faces.push_back({collar.at(b),cap.at(a),cap.at(b)});
      result.curtain_faces+=4U;
    }
    // The current finite terrain sheet is one disk.  Make that requirement
    // explicit instead of flattening every interior DC triangle (which can
    // overlap even for a perfectly embedded non-heightfield surface).
    std::map<std::uint32_t,std::uint32_t> next;
    std::set<std::uint32_t> incoming;
    for(const auto edge:boundary) {
      if(!next.emplace(edge[0],edge[1]).second||!incoming.insert(edge[1]).second)
        return refuse(BccTransitionRequestFailure::invalid_surface);
    }
    if(next.empty()||next.size()!=incoming.size())
      return refuse(BccTransitionRequestFailure::invalid_surface);
    const auto first=next.begin()->first;
    auto cursor=first;
    std::vector<std::uint32_t> loop;
    do {
      loop.push_back(cursor);
      const auto found=next.find(cursor);
      if(found==next.end()||loop.size()>next.size())
        return refuse(BccTransitionRequestFailure::invalid_surface);
      cursor=found->second;
    } while(cursor!=first);
    if(loop.size()!=next.size())return refuse(BccTransitionRequestFailure::invalid_surface);
    Vec3 cap_centre{};
    for(const auto vertex:loop) {
      const auto& p=input.vertices[cap.at(vertex)];
      cap_centre.x+=p.x;cap_centre.y+=p.y;
    }
    cap_centre.x/=static_cast<double>(loop.size());
    cap_centre.y/=static_cast<double>(loop.size());
    cap_centre.z=cap_z;
    const auto centre_id=(std::uint64_t{3}<<62U)|((std::uint64_t{1}<<62U)-1U);
    const auto cap_centre_index=add(centre_id,cap_centre);
    for(const auto edge:boundary)
      input.outer_faces.push_back({cap.at(edge[1]),cap.at(edge[0]),cap_centre_index});
    result.finite_cap_faces=boundary.size();

    std::set<Face> interface;
    for(auto face:core.interface_faces) {
      face=canonical_face(face);
      if(!interface.insert(face).second)
        return refuse(BccTransitionRequestFailure::malformed_interface);
    }
    for(const auto owner:core.logical_owners) {
      const auto keys=world_tetrahedron_vertex_keys(owner);
      bool incident=false;
      for(std::size_t omitted=0U;omitted<4U;++omitted) {
        Face face{};
        std::size_t cursor{};
        for(std::size_t corner=0U;corner<4U;++corner)
          if(corner!=omitted)face[cursor++]=keys[corner];
        incident=incident||interface.contains(canonical_face(face));
      }
      if(!incident)continue;
      // The finite viewer closure is not part of the planetary hierarchy.
      // Leave root-box boundary owners implicit so the explicit local core is
      // strictly inside that artificial curtain instead of sharing it.
      const bool touches_root_boundary=std::ranges::any_of(keys,[](WorldVertexKey key) {
        const auto denominator=std::int64_t{1}<<key.denominator_exponent;
        return key.x==0||key.y==0||key.z==0||key.x==denominator||
               key.y==denominator||key.z==denominator;
      });
      if(touches_root_boundary)continue;
      std::array<std::uint32_t,4> tet{};
      for(std::size_t corner=0U;corner<4U;++corner)
        tet[corner]=add(bcc_transition_hierarchy_vertex_id(keys[corner]),probe_position(keys[corner]));
      input.retained_core_tetrahedra.push_back(tet);
      result.materialized_interface_owners.push_back(owner);
    }
  } catch(const std::out_of_range&) {
    return refuse(BccTransitionRequestFailure::unsupported_address_depth);
  } catch(const std::logic_error&) {
    return refuse(BccTransitionRequestFailure::stable_id_collision);
  }
  if(result.materialized_interface_owners.empty())
    return refuse(BccTransitionRequestFailure::malformed_interface);
  result.hierarchy_interface_faces=core.interface_faces.size();
  result.implicit_far_core_owners=core.logical_owners.size()-
      result.materialized_interface_owners.size();
  result.validation=validate_surface_core_transition_input(input);
  if(!result.validation.accepted)
    return refuse(BccTransitionRequestFailure::invalid_closed_contract);
  result.failure=BccTransitionRequestFailure::none;
  return result;
}

} // namespace tetra::probes
