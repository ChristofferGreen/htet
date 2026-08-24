#include "tetra_core/four_hexahedra.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
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

} // namespace tetra
