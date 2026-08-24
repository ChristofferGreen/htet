#include "tetra_core/implicit_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;

double milliseconds(Clock::time_point start){
  return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}

void benchmark_field(tetra::ImplicitShapeKind kind,std::span<const tetra::Vec3> points){
  tetra::Sphere surface;
  surface.kind=kind;
  std::vector<double> scalar(points.size()),batch(points.size());
  constexpr int repetitions=24;
  auto start=Clock::now();
  for(int repetition=0;repetition<repetitions;++repetition)
    for(std::size_t index=0;index<points.size();++index)
      scalar[index]=surface.signed_distance(points[index]);
  const double scalar_ms=milliseconds(start);
  start=Clock::now();
  for(int repetition=0;repetition<repetitions;++repetition)
    tetra::evaluate_signed_distances(surface,points,batch);
  const double batch_ms=milliseconds(start);
  double maximum_error{},checksum{};
  for(std::size_t index=0;index<points.size();++index){
    maximum_error=std::max(maximum_error,std::abs(scalar[index]-batch[index]));
    checksum+=batch[index];
  }
  std::cout<<"{\"kernel\":\"signed-distance\",\"shape\":\""
           <<tetra::implicit_shape_key(kind)<<"\",\"points\":"<<points.size()
           <<",\"repetitions\":"<<repetitions
           <<",\"scalar_ms\":"<<std::fixed<<std::setprecision(3)<<scalar_ms
           <<",\"batch_ms\":"<<batch_ms
           <<",\"speedup\":"<<scalar_ms/batch_ms
           <<",\"maximum_error\":"<<std::scientific<<maximum_error
           <<",\"checksum\":"<<checksum<<"}\n";
}
}

int main(){
  constexpr std::size_t point_count=1U<<18U;
  std::vector<tetra::Vec3> points;
  points.reserve(point_count);
  for(std::size_t index=0;index<point_count;++index){
    const double value=static_cast<double>(index);
    points.push_back({std::fmod(value*0.6180339887498948,1.4)-0.2,
                      std::fmod(value*0.4142135623730950,1.4)-0.2,
                      std::fmod(value*0.7320508075688772,1.4)-0.2});
  }
  for(const auto kind:{tetra::ImplicitShapeKind::sphere,
                       tetra::ImplicitShapeKind::merging_spheres,
                       tetra::ImplicitShapeKind::cube,
                       tetra::ImplicitShapeKind::capped_cylinder,
                       tetra::ImplicitShapeKind::torus,
                       tetra::ImplicitShapeKind::rounded_cube,
                       tetra::ImplicitShapeKind::perlin_terrain})
    benchmark_field(kind,points);
}
