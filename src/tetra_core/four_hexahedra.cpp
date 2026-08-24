#include "tetra_core/four_hexahedra.hpp"
#include "tetra_core/implicit_surface.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace tetra {
namespace {

[[nodiscard]] BarycentricTwelfths point(
    std::initializer_list<std::pair<std::uint8_t,std::uint8_t>> terms) {
  BarycentricTwelfths result;
  for(const auto [vertex,weight]:terms)result.weights[vertex]=weight;
  return result;
}

[[nodiscard]] std::array<std::uint8_t,3> other_vertices(
    const std::array<std::uint8_t,4>& order,std::uint8_t owner) {
  std::array<std::uint8_t,3> others{};
  std::size_t output{};
  for(const auto vertex:order)
    if(vertex!=owner)others[output++]=vertex;
  return others;
}

struct BarycentricNinetySixths {
  std::array<std::uint16_t,4> weights{};
};

constexpr std::array<std::array<std::uint8_t,4>,6> cube_faces{{
    {{0U,1U,3U,2U}},{{4U,5U,7U,6U}},
    {{0U,1U,5U,4U}},{{2U,3U,7U,6U}},
    {{0U,2U,6U,4U}},{{1U,3U,7U,5U}}}};

[[nodiscard]] std::array<BarycentricNinetySixths,
                         four_hexahedra_field_samples_per_cell>
four_hexahedra_sample_template() {
  std::array<BarycentricNinetySixths,
             four_hexahedra_field_samples_per_cell> result{};
  const auto construction=make_four_hexahedra();
  for(std::size_t hex=0;hex<construction.cells.size();++hex){
    const auto base=hex*15U;
    for(std::size_t corner=0;corner<8U;++corner)
      for(std::size_t vertex=0;vertex<4U;++vertex)
        result[base+corner].weights[vertex]=static_cast<std::uint16_t>(
            construction.cells[hex][corner].weights[vertex]*8U);
    for(std::size_t face=0;face<cube_faces.size();++face)
      for(std::size_t vertex=0;vertex<4U;++vertex){
        std::uint16_t sum{};
        for(const auto corner:cube_faces[face])
          sum=static_cast<std::uint16_t>(
              sum+result[base+corner].weights[vertex]);
        result[base+8U+face].weights[vertex]=
            static_cast<std::uint16_t>(sum/4U);
      }
    for(std::size_t vertex=0;vertex<4U;++vertex){
      std::uint16_t sum{};
      for(std::size_t corner=0;corner<8U;++corner)
        sum=static_cast<std::uint16_t>(
            sum+result[base+corner].weights[vertex]);
      result[base+14U].weights[vertex]=static_cast<std::uint16_t>(sum/8U);
    }
  }
  return result;
}

const auto four_hexahedra_samples=four_hexahedra_sample_template();

[[nodiscard]] std::array<Vec3,four_hexahedra_field_samples_per_cell>
sample_positions(const TetMesh& mesh,const Tetrahedron& tet) {
  std::array<Vec3,four_hexahedra_field_samples_per_cell> result{};
  for(std::size_t sample=0;sample<result.size();++sample){
    std::array<std::pair<VertexId,std::uint16_t>,4> terms{};
    for(std::size_t local=0;local<4U;++local)
      terms[local]={tet.vertices[local],four_hexahedra_samples[sample].weights[local]};
    std::sort(terms.begin(),terms.end(),[](const auto& left,const auto& right){
      return left.first<right.first;
    });
    for(const auto [vertex,weight]:terms){
      if(weight==0U)continue;
      result[sample]=result[sample]+mesh.vertices()[vertex]*
          (static_cast<double>(weight)/96.0);
    }
  }
  return result;
}

[[nodiscard]] bool point_less(Vec3 left,Vec3 right) {
  if(left.x!=right.x)return left.x<right.x;
  if(left.y!=right.y)return left.y<right.y;
  return left.z<right.z;
}

[[nodiscard]] Vec3 cross(Vec3 a,Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}

[[nodiscard]] double dot(Vec3 a,Vec3 b) {
  return a.x*b.x+a.y*b.y+a.z*b.z;
}

void append_polygonized_tetrahedron(
    const Sphere& surface,const std::array<Vec3,4>& points,
    const std::array<double,4>& distances,std::vector<Triangle>& triangles) {
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  std::array<Vec3,4> crossings{};
  std::size_t crossing_count{};
  for(const auto edge:edges){
    if((distances[edge[0]]<0.0)==(distances[edge[1]]<0.0))continue;
    Vec3 first=points[edge[0]],second=points[edge[1]];
    if(point_less(second,first))std::swap(first,second);
    const auto crossing=surface.edge_intersection(first,second);
    const auto same=[&](Vec3 existing){
      const auto delta=existing-crossing;
      return dot(delta,delta)<1.0e-26;
    };
    if(std::ranges::none_of(
           std::span<const Vec3>(crossings).first(crossing_count),same))
      crossings[crossing_count++]=crossing;
  }
  if(crossing_count<3U)return;
  Vec3 centre{};
  for(std::size_t index=0;index<crossing_count;++index)
    centre=centre+crossings[index];
  centre=centre/static_cast<double>(crossing_count);
  const auto outward=surface.normal(centre);
  const Vec3 reference=std::abs(outward.z)<0.9?Vec3{0.0,0.0,1.0}:
                                               Vec3{0.0,1.0,0.0};
  const auto axis_u=cross(reference,outward);
  const auto axis_v=cross(outward,axis_u);
  std::sort(crossings.begin(),
            crossings.begin()+static_cast<std::ptrdiff_t>(crossing_count),
            [&](Vec3 left,Vec3 right){
              const auto left_offset=left-centre,right_offset=right-centre;
              return std::atan2(dot(left_offset,axis_v),dot(left_offset,axis_u))<
                     std::atan2(dot(right_offset,axis_v),dot(right_offset,axis_u));
            });
  for(std::size_t index=1U;index+1U<crossing_count;++index){
    Triangle triangle{crossings[0],crossings[index],crossings[index+1U]};
    auto normal=cross(triangle.b-triangle.a,triangle.c-triangle.a);
    if(dot(normal,normal)<1.0e-28)continue;
    const auto triangle_centre=(triangle.a+triangle.b+triangle.c)/3.0;
    if(dot(normal,surface.normal(triangle_centre))<0.0)
      std::swap(triangle.b,triangle.c);
    triangles.push_back(triangle);
  }
}

void polygonize_four_hexahedra_cell(
    const Sphere& surface,
    const std::array<Vec3,four_hexahedra_field_samples_per_cell>& positions,
    std::span<const double,four_hexahedra_field_samples_per_cell> distances,
    std::vector<Triangle>& triangles) {
  for(std::size_t hex=0;hex<4U;++hex){
    const auto base=hex*15U;
    for(std::size_t face=0;face<cube_faces.size();++face)
      for(std::size_t edge=0;edge<4U;++edge){
        const std::array<std::size_t,4> indices{{
            base+14U,base+8U+face,base+cube_faces[face][edge],
            base+cube_faces[face][(edge+1U)%4U]}};
        std::array<Vec3,4> points{};
        std::array<double,4> values{};
        for(std::size_t corner=0;corner<4U;++corner){
          points[corner]=positions[indices[corner]];
          values[corner]=distances[indices[corner]];
        }
        append_polygonized_tetrahedron(surface,points,values,triangles);
      }
  }
}

[[nodiscard]] auto sample_record_key(
    const FourHexahedraFieldSampleRecord& record) {
  return std::pair{record.logical_owner,record.conforming_cell};
}

} // namespace

FourHexahedra make_four_hexahedra(std::array<std::uint8_t,4> vertex_order) {
  auto sorted=vertex_order;
  std::sort(sorted.begin(),sorted.end());
  if(sorted!=std::array<std::uint8_t,4>{{0U,1U,2U,3U}})
    throw std::invalid_argument("four-hexahedra vertex order is not a permutation");

  FourHexahedra result;
  result.vertex_order=vertex_order;
  for(std::size_t cell=0;cell<result.cells.size();++cell){
    const auto owner=vertex_order[cell];
    const auto others=other_vertices(vertex_order,owner);
    const auto a=others[0];
    const auto b=others[1];
    const auto c=others[2];
    result.cells[cell]={{
        point({{owner,12U}}),
        point({{owner,6U},{a,6U}}),
        point({{owner,6U},{b,6U}}),
        point({{owner,4U},{a,4U},{b,4U}}),
        point({{owner,6U},{c,6U}}),
        point({{owner,4U},{a,4U},{c,4U}}),
        point({{owner,4U},{b,4U},{c,4U}}),
        point({{0U,3U},{1U,3U},{2U,3U},{3U,3U}})}};
  }
  return result;
}

BoundaryQuad four_hexahedra_boundary_quad(
    const FourHexahedra& construction,std::uint8_t owner_vertex,
    std::uint8_t opposite_vertex) {
  if(owner_vertex>=4U||opposite_vertex>=4U||owner_vertex==opposite_vertex)
    throw std::invalid_argument("invalid four-hexahedra boundary vertices");
  const auto owner_position=std::ranges::find(
      construction.vertex_order,owner_vertex);
  if(owner_position==construction.vertex_order.end())
    throw std::invalid_argument("four-hexahedra construction has no owner vertex");
  const auto others=other_vertices(construction.vertex_order,owner_vertex);
  const auto opposite_axis=std::ranges::find(others,opposite_vertex);
  if(opposite_axis==others.end())
    throw std::invalid_argument("four-hexahedra construction has no opposite vertex");

  std::array<unsigned int,2> face_axes{};
  std::size_t output{};
  for(unsigned int axis=0;axis<3U;++axis)
    if(others[axis]!=opposite_vertex)face_axes[output++]=axis;
  const auto corner_index=[&](unsigned int first,unsigned int second){
    return (first<<face_axes[0])|(second<<face_axes[1]);
  };
  const auto cell=static_cast<std::size_t>(
      owner_position-construction.vertex_order.begin());
  const auto& corners=construction.cells[cell];
  return BoundaryQuad{{corners[corner_index(0U,0U)],
                       corners[corner_index(1U,0U)],
                       corners[corner_index(0U,1U)],
                       corners[corner_index(1U,1U)]}};
}

RationalBarycentricSample sample_boundary_quad(
    const BoundaryQuad& quad,std::uint32_t resolution,
    std::uint32_t u,std::uint32_t v) {
  if(resolution==0U||u>resolution||v>resolution)
    throw std::invalid_argument("invalid four-hexahedra boundary sample coordinate");
  const std::uint64_t n=resolution;
  if(n>std::numeric_limits<std::uint64_t>::max()/
           (barycentric_twelfths_denominator*n))
    throw std::invalid_argument("four-hexahedra boundary resolution is too large");
  const std::uint64_t uu=u;
  const std::uint64_t vv=v;
  const std::array<std::uint64_t,4> factors{{
      (n-uu)*(n-vv),uu*(n-vv),(n-uu)*vv,uu*vv}};
  RationalBarycentricSample result;
  result.denominator=barycentric_twelfths_denominator*n*n;
  for(std::size_t corner=0;corner<quad.corners.size();++corner)
    for(std::size_t vertex=0;vertex<result.numerators.size();++vertex)
      result.numerators[vertex]+=
          factors[corner]*quad.corners[corner].weights[vertex];
  return result;
}

void FourHexahedraPatchBuilder::begin_update(std::uint64_t field_revision) {
  if(updating_)throw std::logic_error("four-hexahedra sample update already active");
  updating_=true;
  field_revision_=field_revision;
  last_owner_=invalid_tet;
  record_scratch_.clear();
  record_scratch_.reserve(records_.size());
  metrics_={};
}

void FourHexahedraPatchBuilder::retain_owner(TetId logical_owner) {
  if(!updating_)throw std::logic_error("four-hexahedra sample update is not active");
  if(logical_owner==invalid_tet||
     (last_owner_!=invalid_tet&&logical_owner<=last_owner_))
    throw std::invalid_argument("four-hexahedra owners must be unique and sorted");
  last_owner_=logical_owner;
  const auto begin=std::lower_bound(
      records_.begin(),records_.end(),std::pair{logical_owner,TetId{}},
      [](const auto& record,const auto& key){return sample_record_key(record)<key;});
  for(auto found=begin;found!=records_.end()&&
      found->logical_owner==logical_owner;++found)
    record_scratch_.push_back(*found);
}

std::size_t FourHexahedraPatchBuilder::allocate_samples() {
  if(!free_sample_ranges_.empty()){
    const auto result=free_sample_ranges_.back();
    free_sample_ranges_.pop_back();
    return result;
  }
  const auto result=sample_arena_.size();
  sample_arena_.resize(result+four_hexahedra_field_samples_per_cell);
  return result;
}

void FourHexahedraPatchBuilder::extract_owner(
    const TetMesh& mesh,const Sphere& surface,TetId logical_owner,
    std::span<const TetId> conforming_cells,std::vector<Triangle>& triangles) {
  if(!updating_)throw std::logic_error("four-hexahedra sample update is not active");
  if(logical_owner==invalid_tet||
     (last_owner_!=invalid_tet&&logical_owner<=last_owner_)||
     !std::ranges::is_sorted(conforming_cells)||
     std::ranges::adjacent_find(conforming_cells)!=conforming_cells.end())
    throw std::invalid_argument("invalid four-hexahedra owner cell range");
  last_owner_=logical_owner;
  triangles.reserve(std::max(
      triangles.capacity(),triangles.size()+conforming_cells.size()*24U));
  const auto owner_begin=std::lower_bound(
      records_.begin(),records_.end(),std::pair{logical_owner,TetId{}},
      [](const auto& record,const auto& key){return sample_record_key(record)<key;});
  for(const auto cell:conforming_cells){
    const auto& tet=mesh.tetrahedron(cell);
    const auto found=std::lower_bound(
        owner_begin,records_.end(),std::pair{logical_owner,cell},
        [](const auto& record,const auto& key){return sample_record_key(record)<key;});
    const bool retained=found!=records_.end()&&
        found->logical_owner==logical_owner&&found->conforming_cell==cell;
    FourHexahedraFieldSampleRecord record;
    if(retained)record=*found;
    else{
      record.logical_owner=logical_owner;
      record.conforming_cell=cell;
      record.sample_begin=allocate_samples();
    }
    const auto positions=sample_positions(mesh,tet);
    const bool samples_match=retained&&record.vertices==tet.vertices&&
        record.field_revision==field_revision_;
    auto values=std::span<double,four_hexahedra_field_samples_per_cell>(
        sample_arena_.data()+record.sample_begin,
        four_hexahedra_field_samples_per_cell);
    if(samples_match)metrics_.reused_samples+=values.size();
    else{
      evaluate_signed_distances(surface,positions,values);
      metrics_.evaluated_samples+=values.size();
    }
    record.vertices=tet.vertices;
    record.field_revision=field_revision_;
    record_scratch_.push_back(record);
    polygonize_four_hexahedra_cell(surface,positions,values,triangles);
  }
}

void FourHexahedraPatchBuilder::finish_update() {
  if(!updating_)throw std::logic_error("four-hexahedra sample update is not active");
  std::size_t current{},next{};
  while(current<records_.size()){
    const auto current_key=sample_record_key(records_[current]);
    while(next<record_scratch_.size()&&
          sample_record_key(record_scratch_[next])<current_key)++next;
    if(next==record_scratch_.size()||
       sample_record_key(record_scratch_[next])!=current_key)
      free_sample_ranges_.push_back(records_[current].sample_begin);
    ++current;
  }
  std::sort(free_sample_ranges_.begin(),free_sample_ranges_.end());
  while(!free_sample_ranges_.empty()&&
        free_sample_ranges_.back()+four_hexahedra_field_samples_per_cell==
            sample_arena_.size()){
    sample_arena_.resize(free_sample_ranges_.back());
    free_sample_ranges_.pop_back();
  }
  records_.swap(record_scratch_);
  metrics_.records=records_.size();
  metrics_.arena_slots=sample_arena_.size();
  metrics_.free_slots=
      free_sample_ranges_.size()*four_hexahedra_field_samples_per_cell;
  metrics_.retained_bytes=
      records_.capacity()*sizeof(FourHexahedraFieldSampleRecord)+
      record_scratch_.capacity()*sizeof(FourHexahedraFieldSampleRecord)+
      sample_arena_.capacity()*sizeof(double)+
      free_sample_ranges_.capacity()*sizeof(std::size_t);
  updating_=false;
}

void extract_four_hexahedra_isosurface(
    const TetMesh& mesh,const Sphere& surface,
    std::span<const TetId> conforming_cells,std::vector<Triangle>& triangles) {
  triangles.clear();
  triangles.reserve(conforming_cells.size()*24U);
  FourHexahedraPatchBuilder builder;
  builder.begin_update(0U);
  builder.extract_owner(mesh,surface,TetId{},conforming_cells,triangles);
  builder.finish_update();
}

std::vector<Triangle> extract_four_hexahedra_isosurface(
    const TetMesh& mesh,const Sphere& surface) {
  std::vector<Triangle> triangles;
  extract_four_hexahedra_isosurface(
      mesh,surface,mesh.conforming_volume().addresses(),triangles);
  return triangles;
}

} // namespace tetra
