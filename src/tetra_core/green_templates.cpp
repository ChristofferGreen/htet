#include "tetra_core/green_templates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace tetra {
namespace {

constexpr std::array<std::array<std::uint8_t,2>,6> edge_vertices{{
    {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};

std::uint8_t edge_index(std::uint8_t first,std::uint8_t second){
  if(first>second)std::swap(first,second);
  for(std::uint8_t index=0;index<edge_vertices.size();++index)
    if(edge_vertices[index][0]==first&&edge_vertices[index][1]==second)return index;
  throw std::logic_error("invalid tetrahedron edge");
}

bool even_permutation(const std::array<std::uint8_t,4>& order){
  unsigned int inversions{};
  for(std::size_t first=0;first<order.size();++first)
    for(std::size_t second=first+1;second<order.size();++second)
      inversions+=order[first]>order[second]?1U:0U;
  return (inversions&1U)==0U;
}

struct Point { double x{},y{},z{}; };
constexpr std::array<Point,10> reference_points{{
    {0.0,0.0,1.0},{0.0,1.0,0.0},{1.0,0.0,0.0},{0.0,0.0,0.0},
    {1.0,0.0,1.0},{1.0,1.0,0.0},{2.0,0.0,0.0},{0.0,1.0,1.0},
    {0.0,2.0,0.0},{0.0,0.0,2.0}}};

Point operator-(Point first,Point second){
  return {first.x-second.x,first.y-second.y,first.z-second.z};
}
double dot(Point first,Point second){
  return first.x*second.x+first.y*second.y+first.z*second.z;
}
Point cross(Point first,Point second){
  return {first.y*second.z-first.z*second.y,
          first.z*second.x-first.x*second.z,
          first.x*second.y-first.y*second.x};
}
double determinant(Point a,Point b,Point c,Point d){
  return dot(b-a,cross(c-a,d-a));
}

int affine_dimension(const std::vector<std::uint8_t>& points){
  if(points.empty())return -1;
  std::array<std::array<double,3>,3> basis{};
  int rank=0;
  for(std::size_t index=1;index<points.size()&&rank<3;++index){
    const Point delta=reference_points[points[index]]-reference_points[points[0]];
    std::array<double,3> row{{delta.x,delta.y,delta.z}};
    for(int pivot=0;pivot<rank;++pivot){
      int column=0;
      while(column<3&&std::abs(basis[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(column)])<1.0e-12)
        ++column;
      if(column==3)continue;
      const double scale=row[static_cast<std::size_t>(column)]/
          basis[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(column)];
      for(int component=column;component<3;++component)
        row[static_cast<std::size_t>(component)]-=scale*
            basis[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(component)];
    }
    int column=0;
    while(column<3&&std::abs(row[static_cast<std::size_t>(column)])<1.0e-12)++column;
    if(column==3)continue;
    basis[static_cast<std::size_t>(rank++)]=row;
  }
  return rank;
}

double visibility_sign(const std::vector<std::uint8_t>& facet,
                       std::uint8_t opposite,std::uint8_t point,int dimension,
                       const std::vector<std::uint8_t>& inserted){
  if(dimension==1){
    Point direction{};
    for(const auto candidate:inserted){
      direction=reference_points[candidate]-reference_points[facet[0]];
      if(dot(direction,direction)>1.0e-12)break;
    }
    return dot(reference_points[opposite]-reference_points[facet[0]],direction)*
           dot(reference_points[point]-reference_points[facet[0]],direction);
  }
  if(dimension==2){
    Point normal{};
    for(std::size_t first=1;first<inserted.size();++first)
      for(std::size_t second=first+1;second<inserted.size();++second){
        normal=cross(reference_points[inserted[first]]-reference_points[inserted[0]],
                     reference_points[inserted[second]]-reference_points[inserted[0]]);
        if(dot(normal,normal)>1.0e-12)break;
      }
    const Point edge=reference_points[facet[1]]-reference_points[facet[0]];
    const double inside=dot(cross(edge,reference_points[opposite]-reference_points[facet[0]]),normal);
    const double candidate=dot(cross(edge,reference_points[point]-reference_points[facet[0]]),normal);
    return inside*candidate;
  }
  const double inside=determinant(reference_points[facet[0]],reference_points[facet[1]],
                                  reference_points[facet[2]],reference_points[opposite]);
  const double candidate=determinant(reference_points[facet[0]],reference_points[facet[1]],
                                     reference_points[facet[2]],reference_points[point]);
  return inside*candidate;
}

CompleteGreenTemplate generate(std::uint8_t mask){
  std::vector<std::uint8_t> order;
  for(std::uint8_t point=0;point<10U;++point){
    const auto edge=grande_point_edge[point];
    if(edge==0xffU||(mask&(std::uint8_t{1}<<edge))!=0U)order.push_back(point);
  }
  std::vector<std::uint8_t> inserted;
  std::vector<std::vector<std::uint8_t>> maximal;
  int dimension=-1;
  for(const auto point:order){
    auto expanded=inserted;
    expanded.push_back(point);
    const int expanded_dimension=affine_dimension(expanded);
    if(dimension<0){maximal={{point}};dimension=0;inserted=std::move(expanded);continue;}
    if(expanded_dimension>dimension){
      for(auto& simplex:maximal)simplex.push_back(point);
      ++dimension;
      inserted=std::move(expanded);
      continue;
    }
    if(dimension==0){inserted=std::move(expanded);continue;}
    struct FacetRecord { std::size_t count{}; std::uint8_t opposite{}; };
    std::map<std::vector<std::uint8_t>,FacetRecord> boundary;
    for(const auto& simplex:maximal)
      for(std::size_t removed=0;removed<simplex.size();++removed){
        std::vector<std::uint8_t> facet;
        for(std::size_t index=0;index<simplex.size();++index)
          if(index!=removed)facet.push_back(simplex[index]);
        auto key=facet;
        std::sort(key.begin(),key.end());
        auto& record=boundary[key];
        ++record.count;
        record.opposite=simplex[removed];
      }
    std::vector<std::vector<std::uint8_t>> additions;
    for(const auto& [facet,record]:boundary){
      if(record.count!=1U)continue;
      if(visibility_sign(facet,record.opposite,point,dimension,inserted)>=-1.0e-12)continue;
      auto simplex=facet;
      simplex.push_back(point);
      additions.push_back(std::move(simplex));
    }
    maximal.insert(maximal.end(),additions.begin(),additions.end());
    inserted=std::move(expanded);
  }
  if(dimension!=3)throw std::logic_error("complete green template is not three-dimensional");
  CompleteGreenTemplate result;
  if(maximal.size()>result.tetrahedra.size())
    throw std::logic_error("complete green template exceeds packed capacity");
  for(const auto& simplex:maximal){
    if(simplex.size()!=4U)throw std::logic_error("invalid complete green simplex");
    auto tet=std::array<std::uint8_t,4>{{simplex[0],simplex[1],simplex[2],simplex[3]}};
    if(determinant(reference_points[tet[0]],reference_points[tet[1]],
                   reference_points[tet[2]],reference_points[tet[3]])<0.0)
      std::swap(tet[0],tet[1]);
    result.tetrahedra[result.count++]=tet;
  }
  return result;
}

} // namespace

std::uint8_t permute_green_mask(
    std::uint8_t midpoint_mask,
    const std::array<std::uint8_t,4>& vertex_order){
  std::array<bool,4> seen{};
  for(const auto vertex:vertex_order){
    if(vertex>=seen.size()||seen[vertex])
      throw std::invalid_argument("green-mask vertex order is not a permutation");
    seen[vertex]=true;
  }
  if(!even_permutation(vertex_order))
    throw std::invalid_argument("green-mask vertex order reverses orientation");
  std::uint8_t result{};
  for(std::uint8_t edge=0;edge<edge_vertices.size();++edge){
    const auto canonical=edge_vertices[edge];
    const auto original=edge_index(vertex_order[canonical[0]],vertex_order[canonical[1]]);
    if((midpoint_mask&(std::uint8_t{1}<<original))!=0U)
      result|=std::uint8_t{1}<<edge;
  }
  return result;
}

CanonicalGreenMask canonical_green_mask(std::uint8_t midpoint_mask){
  CanonicalGreenMask result{static_cast<std::uint8_t>(midpoint_mask&63U),{0U,1U,2U,3U}};
  std::array<std::uint8_t,4> order{{0U,1U,2U,3U}};
  do{
    if(!even_permutation(order))continue;
    const auto candidate=permute_green_mask(midpoint_mask,order);
    if(candidate<result.mask||(candidate==result.mask&&order<result.vertex_order))
      result={candidate,order};
  }while(std::next_permutation(order.begin(),order.end()));
  return result;
}

const CompleteGreenTemplate& complete_green_template(std::uint8_t midpoint_mask){
  static const auto templates=[] {
    std::array<CompleteGreenTemplate,64> result{};
    for(std::uint8_t mask=0;mask<64U;++mask)result[mask]=generate(mask);
    return result;
  }();
  return templates[midpoint_mask&63U];
}

} // namespace tetra
