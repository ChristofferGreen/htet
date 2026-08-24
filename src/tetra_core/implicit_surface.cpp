#include "tetra_core/implicit_surface.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <numeric>
#include <optional>
#include <span>

#if defined(__aarch64__) && defined(TETRA_ENABLE_SIMD)
#include <arm_neon.h>
#endif

namespace tetra {
namespace {

double signed_six_volume(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
  const Vec3 ab = b - a, ac = c - a, ad = d - a;
  return ab.x * (ac.y * ad.z - ac.z * ad.y) - ab.y * (ac.x * ad.z - ac.z * ad.x) + ab.z * (ac.x * ad.y - ac.y * ad.x);
}

bool contains_point(const TetMesh& mesh, const Tetrahedron& tet, Vec3 point) {
  const Vec3 a = mesh.vertices().at(tet.vertices[0]), b = mesh.vertices().at(tet.vertices[1]);
  const Vec3 c = mesh.vertices().at(tet.vertices[2]), d = mesh.vertices().at(tet.vertices[3]);
  const double total = signed_six_volume(a, b, c, d);
  if (std::abs(total) < 1e-12) return false;
  const double w0 = signed_six_volume(point, b, c, d) / total;
  const double w1 = signed_six_volume(a, point, c, d) / total;
  const double w2 = signed_six_volume(a, b, point, d) / total;
  const double w3 = signed_six_volume(a, b, c, point) / total;
  constexpr double epsilon = 1e-10;
  return w0 >= -epsilon && w1 >= -epsilon && w2 >= -epsilon && w3 >= -epsilon;
}

double squared_distance_to_segment(Vec3 point, Vec3 first, Vec3 second) {
  const Vec3 edge = second - first;
  const Vec3 offset = point - first;
  const double length_squared = edge.x * edge.x + edge.y * edge.y + edge.z * edge.z;
  const double t = std::clamp((offset.x * edge.x + offset.y * edge.y + offset.z * edge.z) / length_squared, 0.0, 1.0);
  const Vec3 closest{first.x + t * edge.x, first.y + t * edge.y, first.z + t * edge.z};
  const Vec3 delta = point - closest;
  return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
}

double squared_distance_to_triangle(Vec3 point, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 ab = b - a, ac = c - a;
  const Vec3 normal{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
  const double normal_squared = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
  const Vec3 ap = point - a;
  const double signed_plane_distance = normal.x * ap.x + normal.y * ap.y + normal.z * ap.z;
  const Vec3 projected{point.x - normal.x * signed_plane_distance / normal_squared,
                       point.y - normal.y * signed_plane_distance / normal_squared,
                       point.z - normal.z * signed_plane_distance / normal_squared};
  const Vec3 projected_offset = projected - a;
  const double uu = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
  const double uv = ab.x * ac.x + ab.y * ac.y + ab.z * ac.z;
  const double vv = ac.x * ac.x + ac.y * ac.y + ac.z * ac.z;
  const double pu = projected_offset.x * ab.x + projected_offset.y * ab.y + projected_offset.z * ab.z;
  const double pv = projected_offset.x * ac.x + projected_offset.y * ac.y + projected_offset.z * ac.z;
  const double denominator = uu * vv - uv * uv;
  const double u = (vv * pu - uv * pv) / denominator;
  const double v = (uu * pv - uv * pu) / denominator;
  if (u >= 0.0 && v >= 0.0 && u + v <= 1.0) return signed_plane_distance * signed_plane_distance / normal_squared;
  return std::min({squared_distance_to_segment(point, a, b), squared_distance_to_segment(point, b, c), squared_distance_to_segment(point, c, a)});
}

bool sphere_intersects_tetrahedron(const TetMesh& mesh, const Tetrahedron& tet, const Sphere& sphere) {
  if (contains_point(mesh, tet, sphere.centre)) return true;
  constexpr std::array<std::array<std::size_t, 3>, 4> faces{{{{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
  for (const auto& face : faces) {
    const double distance_squared = squared_distance_to_triangle(sphere.centre, mesh.vertices().at(tet.vertices[face[0]]), mesh.vertices().at(tet.vertices[face[1]]), mesh.vertices().at(tet.vertices[face[2]]));
    if (distance_squared <= sphere.radius * sphere.radius) return true;
  }
  return false;
}

double smooth_min(double first,double second,double width){
  const double h=std::clamp(0.5+0.5*(second-first)/width,0.0,1.0);
  return second*(1.0-h)+first*h-width*h*(1.0-h);
}

struct NoiseSample { double value{},dx{},dy{}; };

std::array<double,2> noise_gradient(int ix,int iy){
  std::uint32_t hash=static_cast<std::uint32_t>(ix)*0x8da6b343U^
                     static_cast<std::uint32_t>(iy)*0xd8163841U;
  hash^=hash>>13U;hash*=0x85ebca6bU;hash^=hash>>16U;
  constexpr double diagonal=0.7071067811865475244;
  constexpr std::array<std::array<double,2>,8> gradients{{
      {{1.0,0.0}},{{diagonal,diagonal}},{{0.0,1.0}},{{-diagonal,diagonal}},
      {{-1.0,0.0}},{{-diagonal,-diagonal}},{{0.0,-1.0}},{{diagonal,-diagonal}}}};
  return gradients[hash&7U];
}

NoiseSample gradient_noise_sample(double x,double y){
  const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y));
  const double fx=x-ix,fy=y-iy;
  const auto fade=[](double value){return value*value*value*(value*(value*6.0-15.0)+10.0);};
  const auto fade_derivative=[](double value){
    const double square=value*value,difference=value-1.0;
    return 30.0*square*difference*difference;
  };
  const double u=fade(fx),v=fade(fy);
  const double du=fade_derivative(fx),dv=fade_derivative(fy);
  const auto g00=noise_gradient(ix,iy),g10=noise_gradient(ix+1,iy);
  const auto g01=noise_gradient(ix,iy+1),g11=noise_gradient(ix+1,iy+1);
  const double n00=g00[0]*fx+g00[1]*fy;
  const double n10=g10[0]*(fx-1.0)+g10[1]*fy;
  const double n01=g01[0]*fx+g01[1]*(fy-1.0);
  const double n11=g11[0]*(fx-1.0)+g11[1]*(fy-1.0);
  const double a=n00*(1.0-u)+n10*u,b=n01*(1.0-u)+n11*u;
  const double ax=g00[0]*(1.0-u)+g10[0]*u+(n10-n00)*du;
  const double bx=g01[0]*(1.0-u)+g11[0]*u+(n11-n01)*du;
  const double ay=g00[1]*(1.0-u)+g10[1]*u;
  const double by=g01[1]*(1.0-u)+g11[1]*u;
  return {a*(1.0-v)+b*v,
          ax*(1.0-v)+bx*v,
          ay*(1.0-v)+by*v+(b-a)*dv};
}

double gradient_noise(double x,double y){
  const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y));
  const double fx=x-ix,fy=y-iy;
  const auto fade=[](double value){return value*value*value*(value*(value*6.0-15.0)+10.0);};
  const auto contribution=[](std::array<double,2> gradient,double px,double py){
    return gradient[0]*px+gradient[1]*py;
  };
  const double u=fade(fx),v=fade(fy);
  const double a=contribution(noise_gradient(ix,iy),fx,fy)*(1.0-u)+
      contribution(noise_gradient(ix+1,iy),fx-1.0,fy)*u;
  const double b=contribution(noise_gradient(ix,iy+1),fx,fy-1.0)*(1.0-u)+
      contribution(noise_gradient(ix+1,iy+1),fx-1.0,fy-1.0)*u;
  return a*(1.0-v)+b*v;
}

#if defined(__aarch64__) && defined(TETRA_ENABLE_SIMD)
float64x2_t gradient_noise_pair(float64x2_t x,float64x2_t y){
  double xs[2],ys[2];vst1q_f64(xs,x);vst1q_f64(ys,y);
  int ix[2],iy[2];
  double fx_values[2],fy_values[2];
  double g00x[2],g00y[2],g10x[2],g10y[2];
  double g01x[2],g01y[2],g11x[2],g11y[2];
  for(std::size_t lane=0;lane<2U;++lane){
    ix[lane]=static_cast<int>(std::floor(xs[lane]));
    iy[lane]=static_cast<int>(std::floor(ys[lane]));
    fx_values[lane]=xs[lane]-static_cast<double>(ix[lane]);
    fy_values[lane]=ys[lane]-static_cast<double>(iy[lane]);
    const auto g00=noise_gradient(ix[lane],iy[lane]);
    const auto g10=noise_gradient(ix[lane]+1,iy[lane]);
    const auto g01=noise_gradient(ix[lane],iy[lane]+1);
    const auto g11=noise_gradient(ix[lane]+1,iy[lane]+1);
    g00x[lane]=g00[0];g00y[lane]=g00[1];g10x[lane]=g10[0];g10y[lane]=g10[1];
    g01x[lane]=g01[0];g01y[lane]=g01[1];g11x[lane]=g11[0];g11y[lane]=g11[1];
  }
  const auto fx=vld1q_f64(fx_values),fy=vld1q_f64(fy_values);
  const auto one=vdupq_n_f64(1.0),six=vdupq_n_f64(6.0);
  const auto fifteen=vdupq_n_f64(15.0),ten=vdupq_n_f64(10.0);
  const auto fade=[](float64x2_t value,float64x2_t six_value,
                     float64x2_t fifteen_value,float64x2_t ten_value){
    const auto square=vmulq_f64(value,value);
    const auto cube=vmulq_f64(square,value);
    return vmulq_f64(cube,vaddq_f64(vmulq_f64(value,
        vsubq_f64(vmulq_f64(value,six_value),fifteen_value)),ten_value));
  };
  const auto u=fade(fx,six,fifteen,ten),v=fade(fy,six,fifteen,ten);
  const auto fx_minus_one=vsubq_f64(fx,one),fy_minus_one=vsubq_f64(fy,one);
  const auto dot_gradient=[&](const double* gx,const double* gy,
                              float64x2_t dx,float64x2_t dy){
    return vaddq_f64(vmulq_f64(vld1q_f64(gx),dx),
                     vmulq_f64(vld1q_f64(gy),dy));
  };
  const auto n00=dot_gradient(g00x,g00y,fx,fy);
  const auto n10=dot_gradient(g10x,g10y,fx_minus_one,fy);
  const auto n01=dot_gradient(g01x,g01y,fx,fy_minus_one);
  const auto n11=dot_gradient(g11x,g11y,fx_minus_one,fy_minus_one);
  const auto a=vaddq_f64(vmulq_f64(n00,vsubq_f64(one,u)),vmulq_f64(n10,u));
  const auto b=vaddq_f64(vmulq_f64(n01,vsubq_f64(one,u)),vmulq_f64(n11,u));
  return vaddq_f64(vmulq_f64(a,vsubq_f64(one,v)),vmulq_f64(b,v));
}
#endif

bool has_convex_negative_region(ImplicitShapeKind kind) {
  switch (kind) {
    case ImplicitShapeKind::sphere:
    case ImplicitShapeKind::cube:
    case ImplicitShapeKind::capped_cylinder:
    case ImplicitShapeKind::cone:
    case ImplicitShapeKind::rounded_cube:
      return true;
    case ImplicitShapeKind::merging_spheres:
    case ImplicitShapeKind::perlin_terrain:
    case ImplicitShapeKind::torus:
    case ImplicitShapeKind::gyroid:
      return false;
  }
  return false;
}

double field_lipschitz_bound(const Sphere& shape) {
  switch (shape.kind) {
    case ImplicitShapeKind::perlin_terrain:
      // Four octaves halve their amplitude while doubling frequency, so each
      // contributes the same slope. Eight is a conservative total derivative
      // bound for the four smooth gradient-noise octaves used above.
      return std::sqrt(1.0 + std::pow(8.0 * shape.secondary * shape.frequency, 2.0));
    case ImplicitShapeKind::gyroid:
      // Each normalized partial derivative is the sum of two unit-bounded
      // trigonometric terms.
      return 2.0 * std::sqrt(3.0);
    case ImplicitShapeKind::cone:
      return 1.1;
    case ImplicitShapeKind::sphere:
    case ImplicitShapeKind::merging_spheres:
    case ImplicitShapeKind::cube:
    case ImplicitShapeKind::capped_cylinder:
    case ImplicitShapeKind::torus:
    case ImplicitShapeKind::rounded_cube:
      return 1.0;
  }
  return 1.0;
}

void pack_transaction_frontier(
    AdaptationPlanningCache& cache,std::size_t layer_count,
    std::span<const TetId> logical_owners,
    std::span<const AdaptationCommand> commands){
  cache.transaction_layers.resize(layer_count);
  for(auto& layer:cache.transaction_layers){
    layer.addresses.clear();
    layer.current_status_words.clear();
    layer.desired_mark_words.clear();
    layer.command_words.clear();
  }
  for(const TetId owner:logical_owners){
    const auto depth=tet_depth(owner);
    if(depth<cache.transaction_layers.size())
      cache.transaction_layers[depth].addresses.push_back(owner);
  }
  for(const auto& command:commands){
    const auto depth=tet_depth(command.logical_owner);
    if(depth<cache.transaction_layers.size())
      cache.transaction_layers[depth].addresses.push_back(command.logical_owner);
  }
  for(auto& layer:cache.transaction_layers){
    std::sort(layer.addresses.begin(),layer.addresses.end());
    layer.addresses.erase(std::unique(layer.addresses.begin(),layer.addresses.end()),
                          layer.addresses.end());
    const auto count=layer.addresses.size();
    layer.current_status_words.assign((count+63U)/64U,0U);
    layer.desired_mark_words.assign((count+63U)/64U,0U);
    layer.command_words.assign((count+31U)/32U,0U);
  }
  for(const auto& command:commands){
    const auto depth=tet_depth(command.logical_owner);
    auto& layer=cache.transaction_layers[depth];
    const auto found=std::lower_bound(
        layer.addresses.begin(),layer.addresses.end(),command.logical_owner);
    if(found==layer.addresses.end()||*found!=command.logical_owner)
      throw std::logic_error("packed adaptation command has no frontier record");
    const auto index=static_cast<std::size_t>(found-layer.addresses.begin());
    const auto bit=std::uint64_t{1}<<(index%64U);
    const std::uint64_t encoded=command.kind==AdaptationCommandKind::split?1U:
        (command.kind==AdaptationCommandKind::merge?2U:0U);
    if(command.kind==AdaptationCommandKind::merge)
      layer.current_status_words[index/64U]|=bit;
    if(command.kind==AdaptationCommandKind::split)
      layer.desired_mark_words[index/64U]|=bit;
    layer.command_words[index/32U]|=encoded<<((index%32U)*2U);
  }
}

void order_mark_pass_fine_to_coarse(std::vector<AdaptationCommand>& commands){
  std::sort(commands.begin(),commands.end(),[](const auto& first,const auto& second){
    const auto first_depth=tet_depth(first.logical_owner);
    const auto second_depth=tet_depth(second.logical_owner);
    if(first_depth!=second_depth)return first_depth>second_depth;
    if(first.logical_owner!=second.logical_owner)
      return first.logical_owner<second.logical_owner;
    return first.kind<second.kind;
  });
}

}  // namespace

std::string_view implicit_shape_name(ImplicitShapeKind kind){
  switch(kind){
    case ImplicitShapeKind::sphere:return "Sphere";
    case ImplicitShapeKind::merging_spheres:return "Merging spheres";
    case ImplicitShapeKind::cube:return "Cube";
    case ImplicitShapeKind::capped_cylinder:return "Capped cylinder";
    case ImplicitShapeKind::perlin_terrain:return "Perlin terrain";
    case ImplicitShapeKind::torus:return "Torus";
    case ImplicitShapeKind::cone:return "Cone";
    case ImplicitShapeKind::gyroid:return "Gyroid";
    case ImplicitShapeKind::rounded_cube:return "Rounded cube";
  }
  return "Unknown";
}

std::string_view implicit_shape_key(ImplicitShapeKind kind){
  switch(kind){
    case ImplicitShapeKind::sphere:return "sphere";
    case ImplicitShapeKind::merging_spheres:return "merging-spheres";
    case ImplicitShapeKind::cube:return "cube";
    case ImplicitShapeKind::capped_cylinder:return "capped-cylinder";
    case ImplicitShapeKind::perlin_terrain:return "perlin-terrain";
    case ImplicitShapeKind::torus:return "torus";
    case ImplicitShapeKind::cone:return "cone";
    case ImplicitShapeKind::gyroid:return "gyroid";
    case ImplicitShapeKind::rounded_cube:return "rounded-cube";
  }
  return "unknown";
}

double implicit_shape_default_secondary(ImplicitShapeKind kind){
  return kind==ImplicitShapeKind::merging_spheres?0.17:0.12;
}

double Sphere::signed_distance(Vec3 point) const {
  const double x = point.x - centre.x;
  const double y = point.y - centre.y;
  const double z = point.z - centre.z;
  const double radial=std::sqrt(x*x+z*z);
  switch(kind){
    case ImplicitShapeKind::sphere:return std::sqrt(x*x+y*y+z*z)-radius;
    case ImplicitShapeKind::merging_spheres:{
      const double separation=secondary;
      const auto distance=[&](double offset){
        const double dx=x-offset;return std::sqrt(dx*dx+y*y+z*z)-radius*0.78;};
      return smooth_min(distance(-separation),distance(separation),radius*0.32);
    }
    case ImplicitShapeKind::cube:
    case ImplicitShapeKind::rounded_cube:{
      const double rounding=kind==ImplicitShapeKind::rounded_cube?
          std::min(secondary,radius*0.95):0.0;
      const double half=radius-rounding;
      const double qx=std::abs(x)-half,qy=std::abs(y)-half,qz=std::abs(z)-half;
      const double outside=std::sqrt(std::max(qx,0.0)*std::max(qx,0.0)+
          std::max(qy,0.0)*std::max(qy,0.0)+std::max(qz,0.0)*std::max(qz,0.0));
      return outside+std::min(std::max({qx,qy,qz}),0.0)-rounding;
    }
    case ImplicitShapeKind::capped_cylinder:{
      const double dx=radial-radius*0.72,dy=std::abs(y)-radius;
      return std::min(std::max(dx,dy),0.0)+
          std::sqrt(std::max(dx,0.0)*std::max(dx,0.0)+std::max(dy,0.0)*std::max(dy,0.0));
    }
    case ImplicitShapeKind::perlin_terrain:{
      double height=centre.y;
      double amplitude=secondary,scale=frequency,total=0.0;
      for(int octave=0;octave<4;++octave){
        total+=gradient_noise((point.x-centre.x)*scale,
                              (point.z-centre.z)*scale)*amplitude;
        scale*=2.0;amplitude*=0.5;
      }
      return point.y-(height+total);
    }
    case ImplicitShapeKind::torus:{
      const double q=radial-radius*0.68;
      return std::sqrt(q*q+y*y)-secondary;
    }
    case ImplicitShapeKind::cone:{
      const double half=radius;
      const double normalized=std::clamp((y+half)/(2.0*half),0.0,1.0);
      const double side=radial-radius*0.78*(1.0-normalized);
      return std::max(side,std::abs(y)-half);
    }
    case ImplicitShapeKind::gyroid:{
      const double k=2.0*std::acos(-1.0)*frequency;
      const double gx=point.x-centre.x,gy=point.y-centre.y,gz=point.z-centre.z;
      return (std::sin(k*gx)*std::cos(k*gy)+std::sin(k*gy)*std::cos(k*gz)+
              std::sin(k*gz)*std::cos(k*gx)-secondary)/k;
    }
  }
  return 0.0;
}

void evaluate_signed_distances(
    const Sphere& surface,std::span<const Vec3> points,std::span<double> output) {
  if(output.size()!=points.size())
    throw std::invalid_argument("signed-distance batch output size mismatch");
  std::size_t index{};
#if defined(__aarch64__) && defined(TETRA_ENABLE_SIMD)
  if(surface.kind==ImplicitShapeKind::sphere){
    const auto centre_x=vdupq_n_f64(surface.centre.x);
    const auto centre_y=vdupq_n_f64(surface.centre.y);
    const auto centre_z=vdupq_n_f64(surface.centre.z);
    const auto radius=vdupq_n_f64(surface.radius);
    for(;index+1U<points.size();index+=2U){
      const double xs[2]{points[index].x,points[index+1U].x};
      const double ys[2]{points[index].y,points[index+1U].y};
      const double zs[2]{points[index].z,points[index+1U].z};
      const auto x=vsubq_f64(vld1q_f64(xs),centre_x);
      const auto y=vsubq_f64(vld1q_f64(ys),centre_y);
      const auto z=vsubq_f64(vld1q_f64(zs),centre_z);
      const auto squared=vaddq_f64(vaddq_f64(vmulq_f64(x,x),vmulq_f64(y,y)),
                                   vmulq_f64(z,z));
      double values[2];vst1q_f64(values,vsubq_f64(vsqrtq_f64(squared),radius));
      for(std::size_t lane=0;lane<2U;++lane)
        output[index+lane]=std::abs(values[lane])<1.0e-10
            ?surface.signed_distance(points[index+lane]):values[lane];
    }
  }else if(surface.kind==ImplicitShapeKind::merging_spheres){
    const auto centre_x=vdupq_n_f64(surface.centre.x);
    const auto centre_y=vdupq_n_f64(surface.centre.y);
    const auto centre_z=vdupq_n_f64(surface.centre.z);
    const auto radius=vdupq_n_f64(surface.radius*0.78);
    const auto separation=vdupq_n_f64(surface.secondary);
    const auto width=vdupq_n_f64(surface.radius*0.32);
    const auto zero=vdupq_n_f64(0.0),one=vdupq_n_f64(1.0);
    for(;index+1U<points.size();index+=2U){
      const double xs[2]{points[index].x,points[index+1U].x};
      const double ys[2]{points[index].y,points[index+1U].y};
      const double zs[2]{points[index].z,points[index+1U].z};
      const auto x=vsubq_f64(vld1q_f64(xs),centre_x);
      const auto y=vsubq_f64(vld1q_f64(ys),centre_y);
      const auto z=vsubq_f64(vld1q_f64(zs),centre_z);
      const auto yz=vaddq_f64(vmulq_f64(y,y),vmulq_f64(z,z));
      const auto left=vaddq_f64(x,separation),right=vsubq_f64(x,separation);
      const auto first=vsubq_f64(vsqrtq_f64(vaddq_f64(vmulq_f64(left,left),yz)),radius);
      const auto second=vsubq_f64(vsqrtq_f64(vaddq_f64(vmulq_f64(right,right),yz)),radius);
      auto h=vaddq_f64(vdupq_n_f64(0.5),vmulq_n_f64(vdivq_f64(
          vsubq_f64(second,first),width),0.5));
      h=vminq_f64(one,vmaxq_f64(zero,h));
      const auto value=vsubq_f64(
          vaddq_f64(vmulq_f64(second,vsubq_f64(one,h)),vmulq_f64(first,h)),
          vmulq_f64(width,vmulq_f64(h,vsubq_f64(one,h))));
      double values[2];vst1q_f64(values,value);
      for(std::size_t lane=0;lane<2U;++lane)
        output[index+lane]=std::abs(values[lane])<1.0e-10
            ?surface.signed_distance(points[index+lane]):values[lane];
    }
  }else if(surface.kind==ImplicitShapeKind::cube||
           surface.kind==ImplicitShapeKind::rounded_cube){
    const double rounding=surface.kind==ImplicitShapeKind::rounded_cube
        ?std::min(surface.secondary,surface.radius*0.95):0.0;
    const auto half=vdupq_n_f64(surface.radius-rounding);
    const auto zero=vdupq_n_f64(0.0),rounding_vector=vdupq_n_f64(rounding);
    for(;index+1U<points.size();index+=2U){
      const double xs[2]{points[index].x-surface.centre.x,
                         points[index+1U].x-surface.centre.x};
      const double ys[2]{points[index].y-surface.centre.y,
                         points[index+1U].y-surface.centre.y};
      const double zs[2]{points[index].z-surface.centre.z,
                         points[index+1U].z-surface.centre.z};
      const auto qx=vsubq_f64(vabsq_f64(vld1q_f64(xs)),half);
      const auto qy=vsubq_f64(vabsq_f64(vld1q_f64(ys)),half);
      const auto qz=vsubq_f64(vabsq_f64(vld1q_f64(zs)),half);
      const auto px=vmaxq_f64(qx,zero),py=vmaxq_f64(qy,zero),pz=vmaxq_f64(qz,zero);
      const auto outside=vsqrtq_f64(vaddq_f64(vaddq_f64(vmulq_f64(px,px),
          vmulq_f64(py,py)),vmulq_f64(pz,pz)));
      const auto inside=vminq_f64(vmaxq_f64(qx,vmaxq_f64(qy,qz)),zero);
      const auto value=vsubq_f64(vaddq_f64(outside,inside),rounding_vector);
      double values[2];vst1q_f64(values,value);
      for(std::size_t lane=0;lane<2U;++lane)
        output[index+lane]=std::abs(values[lane])<1.0e-10
            ?surface.signed_distance(points[index+lane]):values[lane];
    }
  }else if(surface.kind==ImplicitShapeKind::capped_cylinder||
           surface.kind==ImplicitShapeKind::torus){
    const auto zero=vdupq_n_f64(0.0);
    for(;index+1U<points.size();index+=2U){
      const double xs[2]{points[index].x-surface.centre.x,
                         points[index+1U].x-surface.centre.x};
      const double ys[2]{points[index].y-surface.centre.y,
                         points[index+1U].y-surface.centre.y};
      const double zs[2]{points[index].z-surface.centre.z,
                         points[index+1U].z-surface.centre.z};
      const auto x=vld1q_f64(xs),y=vld1q_f64(ys),z=vld1q_f64(zs);
      const auto radial=vsqrtq_f64(vaddq_f64(vmulq_f64(x,x),vmulq_f64(z,z)));
      float64x2_t value{};
      if(surface.kind==ImplicitShapeKind::torus){
        const auto q=vsubq_f64(radial,vdupq_n_f64(surface.radius*0.68));
        value=vsubq_f64(vsqrtq_f64(vaddq_f64(vmulq_f64(q,q),vmulq_f64(y,y))),
                        vdupq_n_f64(surface.secondary));
      }else{
        const auto dx=vsubq_f64(radial,vdupq_n_f64(surface.radius*0.72));
        const auto dy=vsubq_f64(vabsq_f64(y),vdupq_n_f64(surface.radius));
        const auto px=vmaxq_f64(dx,zero),py=vmaxq_f64(dy,zero);
        value=vaddq_f64(vminq_f64(vmaxq_f64(dx,dy),zero),
            vsqrtq_f64(vaddq_f64(vmulq_f64(px,px),vmulq_f64(py,py))));
      }
      double values[2];vst1q_f64(values,value);
      for(std::size_t lane=0;lane<2U;++lane)
        output[index+lane]=std::abs(values[lane])<1.0e-10
            ?surface.signed_distance(points[index+lane]):values[lane];
    }
  }else if(surface.kind==ImplicitShapeKind::perlin_terrain){
    for(;index+1U<points.size();index+=2U){
      const double xs[2]{points[index].x-surface.centre.x,
                         points[index+1U].x-surface.centre.x};
      const double zs[2]{points[index].z-surface.centre.z,
                         points[index+1U].z-surface.centre.z};
      auto x=vld1q_f64(xs),z=vld1q_f64(zs);
      auto total=vdupq_n_f64(0.0);
      double amplitude=surface.secondary,scale=surface.frequency;
      for(int octave=0;octave<4;++octave){
        const auto scale_vector=vdupq_n_f64(scale);
        const auto noise=gradient_noise_pair(
            vmulq_f64(x,scale_vector),vmulq_f64(z,scale_vector));
        total=vaddq_f64(total,vmulq_f64(noise,vdupq_n_f64(amplitude)));
        scale*=2.0;amplitude*=0.5;
      }
      const double ys[2]{points[index].y-surface.centre.y,
                         points[index+1U].y-surface.centre.y};
      double values[2];vst1q_f64(values,vsubq_f64(vld1q_f64(ys),total));
      for(std::size_t lane=0;lane<2U;++lane){
        // Classification owns topology. Recheck numerically ambiguous lanes
        // with the scalar oracle so vector rounding cannot change a sign.
        output[index+lane]=std::abs(values[lane])<1.0e-10
            ?surface.signed_distance(points[index+lane]):values[lane];
      }
    }
  }
#endif
  for(;index<points.size();++index)output[index]=surface.signed_distance(points[index]);
}

Vec3 Sphere::normal(Vec3 point) const {
  if(kind==ImplicitShapeKind::sphere){
    const Vec3 offset=point-centre;
    const double length=std::sqrt(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
    return length>1.0e-15?offset/length:Vec3{0.0,1.0,0.0};
  }
  constexpr double epsilon=1.0e-5;
  if(kind==ImplicitShapeKind::perlin_terrain){
    double height_dx=0.0,height_dz=0.0;
    double amplitude=secondary,scale=frequency;
    for(int octave=0;octave<4;++octave){
      const auto sample=gradient_noise_sample((point.x-centre.x)*scale,
                                               (point.z-centre.z)*scale);
      height_dx+=sample.dx*amplitude*scale;
      height_dz+=sample.dy*amplitude*scale;
      scale*=2.0;amplitude*=0.5;
    }
    Vec3 gradient{-height_dx,1.0,-height_dz};
    const double length=std::sqrt(gradient.x*gradient.x+gradient.y*gradient.y+
                                  gradient.z*gradient.z);
    return length>1.0e-15?gradient/length:Vec3{0.0,1.0,0.0};
  }
  Vec3 gradient{signed_distance({point.x+epsilon,point.y,point.z})-
                    signed_distance({point.x-epsilon,point.y,point.z}),
                signed_distance({point.x,point.y+epsilon,point.z})-
                    signed_distance({point.x,point.y-epsilon,point.z}),
                signed_distance({point.x,point.y,point.z+epsilon})-
                    signed_distance({point.x,point.y,point.z-epsilon})};
  const double length=std::sqrt(gradient.x*gradient.x+gradient.y*gradient.y+gradient.z*gradient.z);
  return length>1.0e-15?gradient/length:Vec3{0.0,1.0,0.0};
}

Vec3 Sphere::edge_intersection(Vec3 first,Vec3 second) const {
  if(kind==ImplicitShapeKind::sphere){
    const Vec3 offset=first-centre,direction=second-first;
    const double a=direction.x*direction.x+direction.y*direction.y+direction.z*direction.z;
    const double b=2.0*(offset.x*direction.x+offset.y*direction.y+offset.z*direction.z);
    const double c=offset.x*offset.x+offset.y*offset.y+offset.z*offset.z-radius*radius;
    const double discriminant=std::max(0.0,b*b-4.0*a*c);
    const double first_root=(-b-std::sqrt(discriminant))/(2.0*a);
    const double second_root=(-b+std::sqrt(discriminant))/(2.0*a);
    const double t=first_root>=0.0&&first_root<=1.0?first_root:second_root;
    return first+direction*t;
  }
  double first_distance=signed_distance(first);
  for(int iteration=0;iteration<40;++iteration){
    const Vec3 middle=(first+second)/2.0;
    const double middle_distance=signed_distance(middle);
    if((first_distance<=0.0)==(middle_distance<=0.0)){
      first=middle;first_distance=middle_distance;
    }else{second=middle;}
  }
  return (first+second)/2.0;
}

Vec3 Sphere::project_to_surface(Vec3 point) const {
  if(kind==ImplicitShapeKind::sphere){
    const Vec3 offset=point-centre;
    const double length=std::sqrt(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
    return length>1.0e-15?centre+offset*(radius/length):point;
  }
  for(int iteration=0;iteration<12;++iteration){
    const double distance=signed_distance(point);
    if(std::abs(distance)<1.0e-10)break;
    point=point-normal(point)*distance;
  }
  return point;
}

SurfaceRelation classify_tetrahedron_cached(const TetMesh& mesh,TetId tet,const Sphere& sphere,
                                             std::span<const double> vertex_distances,
                                             std::size_t* exact_evaluations=nullptr) {
  const auto& vertices = mesh.tetrahedron(tet).vertices;
  bool has_negative = false;
  bool has_positive = false;
  Vec3 centre{};
  for (std::size_t index=0;index<vertices.size();++index) {
    const VertexId vertex=vertices[index];
    const Vec3 point=mesh.vertices().at(vertex);
    double distance{};
    if(vertex_distances.empty()){
      distance=sphere.signed_distance(point);
      if(exact_evaluations)++*exact_evaluations;
    }else distance=vertex_distances[vertex];
    if (std::abs(distance) <= 1.0e-12) return SurfaceRelation::intersecting;
    has_negative |= distance < 0.0;
    has_positive |= distance > 0.0;
    centre=centre+point;
  }
  if (has_negative && has_positive) return SurfaceRelation::intersecting;
  if (has_negative && has_convex_negative_region(sphere.kind)) return SurfaceRelation::inside;
  if (sphere.kind==ImplicitShapeKind::sphere) {
  // A same-sign test misses both a sphere contained by a tetrahedron and a
  // sphere that merely clips one of its faces.  Use the closest point on the
  // tetrahedron boundary for the conservative sphere-versus-tetrahedron test.
    if (sphere_intersects_tetrahedron(mesh, mesh.tetrahedron(tet), sphere)) return SurfaceRelation::intersecting;
    return SurfaceRelation::outside;
  }

  // A sign test at only the vertices can miss a feature enclosed by or passing
  // through a tetrahedron.  The field's Lipschitz bound and a bounding sphere
  // around the cell prove when its sign cannot change anywhere in the cell.
  // This is substantially tighter than comparing every vertex with the
  // longest edge and avoids refining the whole interior of convex shapes.
  centre=centre/4.0;
  double radius_squared=0.0;
  for(const VertexId vertex:vertices){
    const Vec3 delta=mesh.vertices().at(vertex)-centre;
    radius_squared=std::max(radius_squared,
        delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
  }
  const double centre_distance=sphere.signed_distance(centre);
  if(exact_evaluations)++*exact_evaluations;
  const double uncertainty=field_lipschitz_bound(sphere)*std::sqrt(radius_squared);
  if(centre_distance>uncertainty)return SurfaceRelation::outside;
  if(centre_distance<-uncertainty)return SurfaceRelation::inside;
  return SurfaceRelation::intersecting;
}

SurfaceRelation classify_tetrahedron(const TetMesh& mesh,TetId tet,const Sphere& sphere){
  return classify_tetrahedron_cached(mesh,tet,sphere,{});
}

namespace {
struct CameraPoint { double x{},y{},depth{}; };

double projected_camera_diameter(
    const std::array<CameraPoint,4>& points,
    const PreparedCameraProjection& camera) {
  // A tetrahedron is convex, so it is outside a frustum half-space exactly
  // when all four vertices are outside that plane. These linear plane tests
  // remain nested for every red child and derived green tetrahedron.
  const auto all_outside=[&](auto signed_inside){
    return std::ranges::all_of(points,[&](const CameraPoint& point){
      return signed_inside(point)<0.0;
    });
  };
  if(all_outside([](const CameraPoint& point){return point.depth;})||
     all_outside([&](const CameraPoint& point){
       return point.x+point.depth*camera.horizontal_tangent;
     })||all_outside([&](const CameraPoint& point){
       return -point.x+point.depth*camera.horizontal_tangent;
     })||all_outside([&](const CameraPoint& point){
       return point.y+point.depth*camera.tangent;
     })||all_outside([&](const CameraPoint& point){
       return -point.y+point.depth*camera.tangent;
     }))return 0.0;
  if(std::ranges::any_of(points,[](const CameraPoint& point){
       return point.depth<=1.0e-12;
     }))return camera.viewport_height_pixels;

  std::array<std::array<double,2>,4> projected{};
  for(std::size_t index=0;index<points.size();++index)
    projected[index]={{camera.focal_length*points[index].x/points[index].depth,
                       camera.focal_length*points[index].y/points[index].depth}};
  double diameter_squared{};
  for(std::size_t first=0;first<projected.size();++first)
    for(std::size_t second=first+1;second<projected.size();++second){
      const double dx=projected[first][0]-projected[second][0];
      const double dy=projected[first][1]-projected[second][1];
      diameter_squared=std::max(diameter_squared,dx*dx+dy*dy);
    }
  return std::sqrt(diameter_squared);
}
} // namespace

PreparedCameraProjection prepare_camera_projection(const Camera& camera) {
  const auto dot=[](Vec3 first,Vec3 second){
    return first.x*second.x+first.y*second.y+first.z*second.z;
  };
  const auto cross=[](Vec3 first,Vec3 second){
    return Vec3{first.y*second.z-first.z*second.y,
                first.z*second.x-first.x*second.z,
                first.x*second.y-first.y*second.x};
  };
  const auto normalize=[&](Vec3 value){
    const double length=std::sqrt(dot(value,value));
    return length>1.0e-15?value/length:Vec3{};
  };
  PreparedCameraProjection result;
  result.position=camera.position;
  result.forward=normalize(camera.forward);
  result.right=normalize(cross(result.forward,camera.up));
  result.up=normalize(cross(result.right,result.forward));
  result.tangent=std::tan(camera.vertical_fov_radians*0.5);
  result.horizontal_tangent=result.tangent*camera.aspect_ratio;
  result.viewport_height_pixels=camera.viewport_height_pixels;
  result.focal_length=(camera.viewport_height_pixels*0.5)/result.tangent;
  return result;
}

double projected_tetrahedron_diameter(
    const TetMesh& mesh,TetId tet,const PreparedCameraProjection& camera) {
  const auto& vertices=mesh.tetrahedron(tet).vertices;
  const auto dot=[](Vec3 first,Vec3 second){
    return first.x*second.x+first.y*second.y+first.z*second.z;
  };
  std::array<CameraPoint,4> points{};
  for(std::size_t index=0;index<vertices.size();++index){
    const Vec3 view=mesh.vertices().at(vertices[index])-camera.position;
    points[index]={dot(view,camera.right),dot(view,camera.up),dot(view,camera.forward)};
  }
  return projected_camera_diameter(points,camera);
}

double projected_tetrahedron_diameter(
    const TetMesh& mesh,TetId tet,const Camera& camera) {
  return projected_tetrahedron_diameter(mesh,tet,prepare_camera_projection(camera));
}

void projected_tetrahedron_diameters(
    const TetMesh& mesh,std::span<const TetId> tetrahedra,
    const PreparedCameraProjection& camera,std::span<double> output) {
  if(output.size()!=tetrahedra.size())
    throw std::invalid_argument("projection batch output size mismatch");
  for(std::size_t index=0;index<tetrahedra.size();++index)
    output[index]=projected_tetrahedron_diameter(mesh,tetrahedra[index],camera);
}

std::vector<TetId> mark_oversized_intersections(const TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold) {
  std::vector<TetId> marked;
  const std::span<const TetId> candidates=mesh.subdivision_method()==
          SubdivisionMethod::bcc_red_green
      ?std::span<const TetId>{mesh.logical_red_owners()}
      :mesh.conforming_volume().addresses();
  for (const TetId id : candidates) {
    if (classify_tetrahedron(mesh, id, sphere) == SurfaceRelation::intersecting &&
        projected_tetrahedron_diameter(mesh, id, camera) > pixel_threshold) marked.push_back(id);
  }
  return marked;
}

AdaptiveResult refine_to_sphere(TetMesh& mesh, const Sphere& sphere, const Camera& camera,
                                double pixel_threshold, unsigned int maximum_depth,
                                ImplicitValueCache* value_cache) {
  AdaptiveResult result;
  const unsigned int increment=subdivision_depth_increment(mesh.subdivision_method());
  ImplicitValueCache local_cache;
  auto& cache=value_cache?*value_cache:local_cache;
  const auto same_surface=[](const Sphere& first,const Sphere& second){
    return first.centre.x==second.centre.x&&first.centre.y==second.centre.y&&
        first.centre.z==second.centre.z&&first.radius==second.radius&&
        first.kind==second.kind&&first.secondary==second.secondary&&
        first.frequency==second.frequency;
  };
  if(!cache.has_sampled_surface||!same_surface(cache.sampled_surface,sphere)){
    cache.vertex_distances.clear();
    cache.sampled_surface=sphere;
    cache.has_sampled_surface=true;
  }
  auto& vertex_distances=cache.vertex_distances;
  constexpr double unevaluated=std::numeric_limits<double>::quiet_NaN();
  while(true){
    vertex_distances.resize(mesh.vertices().size(),unevaluated);
    std::vector<TetId> oversized;
    const std::span<const TetId> candidates=mesh.subdivision_method()==
            SubdivisionMethod::bcc_red_green
        ?std::span<const TetId>{mesh.logical_red_owners()}
        :mesh.conforming_volume().addresses();
    for(const TetId id:candidates){
      for(const VertexId vertex:mesh.tetrahedron(id).vertices)
        if(std::isnan(vertex_distances[vertex]))
          vertex_distances[vertex]=sphere.signed_distance(mesh.vertices()[vertex]);
      if(classify_tetrahedron_cached(mesh,id,sphere,vertex_distances)==
             SurfaceRelation::intersecting&&
         projected_tetrahedron_diameter(mesh,id,camera)>pixel_threshold)
        oversized.push_back(id);
    }
    std::vector<TetId> marked;
    marked.reserve(oversized.size());
    for(const auto id:oversized)if(mesh.refinement_depth(id)+increment<=maximum_depth)marked.push_back(id);
    if(marked.empty()){
      result.reached_depth_limit=!oversized.empty();
      return result;
    }
    if(!mesh.refine_selected_binary(marked)){
      result.reached_depth_limit=true;
      return result;
    }
    result.refined_leaves += marked.size();
    ++result.iterations;
  }
}

AdaptationPlan plan_adaptation(const TetMesh& mesh,const Sphere& sphere,
                               const Camera& camera,double pixel_threshold,
                               unsigned int maximum_depth,
                               const AdaptationConfiguration& configuration,
                               std::uint64_t field_revision,
                               AdaptationPlanningCache* planning_cache) {
  AdaptationPlan plan;
  const auto plan_start=std::chrono::steady_clock::now();
  const auto elapsed_ms=[](auto start){
    return std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
  };
  plan.base_revision=mesh.revision();
  plan.field_revision=field_revision;
  plan.configuration=configuration;
  plan.supported=implemented(configuration)&&
      (configuration.lod_update==LodUpdateStrategy::transactional_active_cut||
       configuration.lod_update==LodUpdateStrategy::saturated_clusters)&&
      mesh.subdivision_method()==SubdivisionMethod::bcc_red_green&&
      mesh.transition_strategy()==configuration.transition_strategy;
  if(!plan.supported||pixel_threshold<=0.0){
    if(planning_cache)
      pack_transaction_frontier(*planning_cache,mesh.layers().size(),
                                mesh.logical_red_owners(),{});
    return plan;
  }
  const auto prepared_camera=prepare_camera_projection(camera);

  AdaptationPlanningCache local_planning_cache;
  auto& summaries=planning_cache?*planning_cache:local_planning_cache;
  const auto same_surface=[](const Sphere& first,const Sphere& second){
    return first.centre.x==second.centre.x&&first.centre.y==second.centre.y&&
        first.centre.z==second.centre.z&&first.radius==second.radius&&
        first.kind==second.kind&&first.secondary==second.secondary&&
        first.frequency==second.frequency;
  };
  const auto same_camera=[](const Camera& first,const Camera& second){
    return first.position.x==second.position.x&&first.position.y==second.position.y&&
        first.position.z==second.position.z&&
        first.vertical_fov_radians==second.vertical_fov_radians&&
        first.viewport_height_pixels==second.viewport_height_pixels&&
        first.forward.x==second.forward.x&&first.forward.y==second.forward.y&&
        first.forward.z==second.forward.z&&first.up.x==second.up.x&&
        first.up.y==second.up.y&&first.up.z==second.up.z&&
        first.aspect_ratio==second.aspect_ratio;
  };
  const auto split_origin_matches=[&]{
    return planning_cache&&summaries.has_split_pose&&
        summaries.split_pose_field_revision==field_revision&&
        summaries.split_pose_configuration==configuration&&
        summaries.split_pose_pixel_threshold==pixel_threshold&&
        summaries.split_pose_maximum_depth==maximum_depth&&
        same_surface(summaries.split_pose_surface,sphere)&&
        summaries.split_pose_camera.position.x==camera.position.x&&
        summaries.split_pose_camera.position.y==camera.position.y&&
        summaries.split_pose_camera.position.z==camera.position.z;
  };
  if(planning_cache){
    const bool translated=summaries.has_last_request_origin&&
        (summaries.last_request_origin.x!=camera.position.x||
         summaries.last_request_origin.y!=camera.position.y||
         summaries.last_request_origin.z!=camera.position.z);
    const bool rotated=summaries.has_last_request_origin&&!translated&&
        (summaries.last_request_forward.x!=camera.forward.x||
         summaries.last_request_forward.y!=camera.forward.y||
         summaries.last_request_forward.z!=camera.forward.z||
         summaries.last_request_up.x!=camera.up.x||
         summaries.last_request_up.y!=camera.up.y||
         summaries.last_request_up.z!=camera.up.z);
    if(translated||rotated)summaries.pose_merge_pending=true;
    summaries.has_last_request_origin=true;
    summaries.last_request_origin=camera.position;
    summaries.last_request_forward=camera.forward;
    summaries.last_request_up=camera.up;
    if(summaries.has_split_pose&&!split_origin_matches())
      summaries.has_split_pose=false;
  }
  if(planning_cache&&summaries.has_stationary_no_change&&
     summaries.stationary_mesh_revision==mesh.revision()&&
     summaries.stationary_field_revision==field_revision&&
     summaries.stationary_pixel_threshold==pixel_threshold&&
     summaries.stationary_maximum_depth==maximum_depth&&
     summaries.stationary_configuration==configuration&&
     same_surface(summaries.stationary_surface,sphere)&&
     same_camera(summaries.stationary_camera,camera)){
    pack_transaction_frontier(summaries,mesh.layers().size(),
                              mesh.logical_red_owners(),{});
    return plan;
  }
  const auto remember_stationary_result=[&](bool no_change){
    if(!planning_cache)return;
    summaries.has_stationary_no_change=no_change;
    if(!no_change)return;
    summaries.stationary_mesh_revision=mesh.revision();
    summaries.stationary_field_revision=field_revision;
    summaries.stationary_surface=sphere;
    summaries.stationary_camera=camera;
    summaries.stationary_configuration=configuration;
    summaries.stationary_pixel_threshold=pixel_threshold;
    summaries.stationary_maximum_depth=maximum_depth;
  };
  const auto apply_scheduler=[&](std::span<const TetId> owners){
    if(configuration.update_scheduler==UpdateScheduler::classify_and_stream||
       plan.commands.empty())return;
    const auto kind=plan.commands.front().kind;
    auto& queue=kind==AdaptationCommandKind::split
        ?summaries.split_queue:summaries.merge_queue;
    for(const auto& command:plan.commands){
      const bool already=std::ranges::any_of(queue,[&](const auto& entry){
        return entry.address==command.logical_owner&&
            entry.state_revision==mesh.revision();
      });
      if(!already){
        queue.push_back({command.logical_owner,mesh.revision(),0.0});
        ++plan.scheduler_queue_pushes;
      }
    }
    // Stable address is the secondary key. Camera priority is deliberately
    // recomputed only when an entry reaches the retained queue front.
    std::sort(queue.begin(),queue.end(),[](const auto& left,const auto& right){
      if(left.last_priority!=right.last_priority)
        return left.last_priority>right.last_priority;
      return left.address<right.address;
    });
    std::size_t stale{};
    for(auto& entry:queue){
      if(entry.state_revision!=mesh.revision()){
        ++plan.scheduler_stale_pops;++stale;continue;
      }
      entry.last_priority=projected_tetrahedron_diameter(
          mesh,entry.address,prepared_camera);
      ++plan.scheduler_priority_recomputations;
      ++plan.scheduler_useful_pops;
    }
    if(configuration.update_scheduler==UpdateScheduler::hybrid_queued_blocks){
      std::vector<std::uint64_t> blocks;
      blocks.reserve(plan.commands.size());
      for(const auto& command:plan.commands)
        blocks.push_back((static_cast<std::uint64_t>(tet_depth(command.logical_owner))<<56U)|
                         (command.logical_owner>>6U));
      std::sort(blocks.begin(),blocks.end());
      blocks.erase(std::unique(blocks.begin(),blocks.end()),blocks.end());
      plan.scheduler_block_streams=blocks.size();
    }
    if(queue.size()>owners.size()/2U+1U||stale>owners.size()/4U){
      ++plan.scheduler_fallbacks;
      queue.clear();
    }
  };

  struct Candidate { TetId address{invalid_tet}; double priority{}; };
  std::vector<Candidate> splits;
  const auto& logical=mesh.logical_red_owners();
  plan.logical_candidates=logical.size();
  const bool has_overdepth=std::ranges::any_of(logical,[&](TetId owner){
    return tet_depth(owner)>maximum_depth;
  });
  const unsigned int increment=subdivision_depth_increment(mesh.subdivision_method());
  const double split_threshold=pixel_threshold*configuration.split_hysteresis;
  if(configuration.candidate_traversal==CandidateTraversal::spatial_runs&&
     (summaries.spatial_index_active_revision!=mesh.revision()||
      summaries.spatial_index_field_revision!=field_revision)){
    const auto index_start=std::chrono::steady_clock::now();
    constexpr std::size_t run_size=64U;
    summaries.spatial_runs.clear();
    summaries.spatial_runs.reserve((logical.size()+run_size-1U)/run_size);
    const double lipschitz=field_lipschitz_bound(sphere);
    for(std::size_t begin=0;begin<logical.size();begin+=run_size){
      SpatialOwnerRun run;
      run.begin=static_cast<std::uint32_t>(begin);
      run.count=static_cast<std::uint32_t>(
          std::min(run_size,logical.size()-begin));
      run.minimum={std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity()};
      run.maximum={-std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()};
      for(std::size_t index=begin;index<begin+run.count;++index){
        for(const VertexId vertex:mesh.tetrahedron(logical[index]).vertices){
          const auto& point=mesh.vertices()[vertex];
          run.minimum.x=std::min(run.minimum.x,point.x);
          run.minimum.y=std::min(run.minimum.y,point.y);
          run.minimum.z=std::min(run.minimum.z,point.z);
          run.maximum.x=std::max(run.maximum.x,point.x);
          run.maximum.y=std::max(run.maximum.y,point.y);
          run.maximum.z=std::max(run.maximum.z,point.z);
        }
      }
      const Vec3 centre=(run.minimum+run.maximum)/2.0;
      const Vec3 radius=run.maximum-centre;
      const double uncertainty=lipschitz*std::sqrt(
          radius.x*radius.x+radius.y*radius.y+radius.z*radius.z);
      const double value=sphere.signed_distance(centre);
      ++plan.exact_field_evaluations;
      run.field_minimum=value-uncertainty;
      run.field_maximum=value+uncertainty;
      summaries.spatial_runs.push_back(run);
    }
    summaries.spatial_index_active_revision=mesh.revision();
    summaries.spatial_index_field_revision=field_revision;
    plan.spatial_index_build_ms=elapsed_ms(index_start);
  }
  if(configuration.candidate_traversal==CandidateTraversal::spatial_runs){
    plan.spatial_index_bytes=summaries.spatial_runs.capacity()*sizeof(SpatialOwnerRun);
    plan.spatial_run_count=summaries.spatial_runs.size();
  }
  if(configuration.candidate_traversal==CandidateTraversal::hierarchy_bounds&&
     (summaries.field_revision!=field_revision||
      summaries.resident_revision!=mesh.resident_revision()||
      summaries.pinned_revision!=mesh.pinned_revision())){
    const auto summary_start=std::chrono::steady_clock::now();
    summaries.layers.clear();
    summaries.layers.resize(mesh.layers().size());
    const double lipschitz=field_lipschitz_bound(sphere);
    for(std::size_t depth=0;depth<mesh.layers().size();++depth){
      const auto& records=mesh.layers()[depth].tetrahedra;
      auto& summary=summaries.layers[depth];
      summary.addresses.reserve(records.size());
      summary.spatial_minimum.reserve(records.size());
      summary.spatial_maximum.reserve(records.size());
      summary.field_minimum.reserve(records.size());
      summary.field_maximum.reserve(records.size());
      summary.deepest_resident_depth.reserve(records.size());
      summary.deepest_active_depth.reserve(records.size());
      for(std::size_t index=0;index<records.size();++index){
        if(records[index].transition_parent!=invalid_tet)continue;
        Vec3 centre{};
        Vec3 minimum{std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity()};
        Vec3 maximum{-std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity()};
        for(const VertexId vertex:records[index].vertices){
          centre=centre+mesh.vertices()[vertex];
          const auto& point=mesh.vertices()[vertex];
          minimum.x=std::min(minimum.x,point.x);
          minimum.y=std::min(minimum.y,point.y);
          minimum.z=std::min(minimum.z,point.z);
          maximum.x=std::max(maximum.x,point.x);
          maximum.y=std::max(maximum.y,point.y);
          maximum.z=std::max(maximum.z,point.z);
        }
        centre=centre/4.0;
        double radius_squared=0.0;
        for(const VertexId vertex:records[index].vertices){
          const auto offset=mesh.vertices()[vertex]-centre;
          radius_squared=std::max(radius_squared,
              offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
        }
        const double value=sphere.signed_distance(centre);
        ++plan.exact_field_evaluations;
        const double uncertainty=lipschitz*std::sqrt(radius_squared);
        summary.addresses.push_back(records[index].address);
        summary.spatial_minimum.push_back(minimum);
        summary.spatial_maximum.push_back(maximum);
        summary.field_minimum.push_back(value-uncertainty);
        summary.field_maximum.push_back(value+uncertainty);
        summary.deepest_resident_depth.push_back(static_cast<unsigned int>(depth));
        summary.deepest_active_depth.push_back(0U);
      }
      summary.pinned_descendant_words.assign((summary.addresses.size()+63U)/64U,0U);
      for(std::size_t index=0;index<summary.addresses.size();++index)
        if(mesh.has_pinned_descendant(summary.addresses[index]))
          summary.pinned_descendant_words[index/64U]|=
              std::uint64_t{1}<<(index%64U);
    }
    const auto summary_position=[&](TetId address)->std::optional<std::size_t>{
      const auto depth=tet_depth(address);
      if(depth>=summaries.layers.size())return std::nullopt;
      const auto& layer=summaries.layers[depth];
      const auto found=std::lower_bound(layer.addresses.begin(),layer.addresses.end(),address);
      if(found==layer.addresses.end()||*found!=address)return std::nullopt;
      return static_cast<std::size_t>(found-layer.addresses.begin());
    };
    for(std::size_t depth=summaries.layers.size();depth-->3U;){
      const auto& child_layer=summaries.layers[depth];
      for(std::size_t index=0;index<child_layer.addresses.size();++index){
        const TetId child=child_layer.addresses[index];
        const TetId parent=make_tet_id(tet_root(child),tet_path(child)>>3U);
        const auto parent_index=summary_position(parent);
        if(!parent_index)continue;
        auto& parent_layer=summaries.layers[tet_depth(parent)];
        parent_layer.deepest_resident_depth[*parent_index]=std::max(
            parent_layer.deepest_resident_depth[*parent_index],
            child_layer.deepest_resident_depth[index]);
      }
    }
    summaries.field_revision=field_revision;
    summaries.resident_revision=mesh.resident_revision();
    summaries.pinned_revision=mesh.pinned_revision();
    summaries.active_revision=std::numeric_limits<std::uint64_t>::max();
    summaries.resident_red_records=static_cast<std::size_t>(std::accumulate(
        summaries.layers.begin(),summaries.layers.end(),std::size_t{},
        [](std::size_t count,const AdaptationSummaryLayer& layer){
          return count+layer.addresses.size();
        }));
    summaries.resident_vertices=mesh.vertices().size();
    plan.summary_build_ms=elapsed_ms(summary_start);
  }
  if(configuration.candidate_traversal==CandidateTraversal::hierarchy_bounds&&
     summaries.active_revision!=mesh.revision()){
    const auto active_summary_start=std::chrono::steady_clock::now();
    for(auto& layer:summaries.layers)
      std::fill(layer.deepest_active_depth.begin(),layer.deepest_active_depth.end(),0U);
    for(const TetId owner:logical){
      TetId descendant=owner;
      while(true){
        const auto depth=tet_depth(descendant);
        if(depth<summaries.layers.size()){
          auto& layer=summaries.layers[depth];
          const auto found=std::lower_bound(
              layer.addresses.begin(),layer.addresses.end(),descendant);
          if(found!=layer.addresses.end()&&*found==descendant){
            const auto index=static_cast<std::size_t>(found-layer.addresses.begin());
            layer.deepest_active_depth[index]=std::max(
                layer.deepest_active_depth[index],tet_depth(owner));
          }
        }
        if(depth<3U)break;
        descendant=make_tet_id(tet_root(descendant),tet_path(descendant)>>3U);
      }
    }
    summaries.active_revision=mesh.revision();
    plan.summary_build_ms+=elapsed_ms(active_summary_start);
  }
  const auto summary_interval=[&](TetId owner)->std::optional<std::array<double,2>>{
    if(configuration.candidate_traversal!=CandidateTraversal::hierarchy_bounds)
      return std::nullopt;
    const auto depth=tet_depth(owner);
    if(depth>=mesh.layers().size()||depth>=summaries.layers.size())return std::nullopt;
    const auto& summary=summaries.layers[depth];
    const auto found=std::lower_bound(summary.addresses.begin(),summary.addresses.end(),owner);
    if(found==summary.addresses.end()||*found!=owner)return std::nullopt;
    const auto index=static_cast<std::size_t>(found-summary.addresses.begin());
    if(index>=summary.field_minimum.size())return std::nullopt;
    return std::array<double,2>{{summary.field_minimum[index],summary.field_maximum[index]}};
  };
  const auto classify_owner=[&](TetId owner){
    ++plan.field_classifications;
    if(const auto interval=summary_interval(owner);
       interval&&((*interval)[0]>0.0||(*interval)[1]<0.0)){
      ++plan.field_subtrees_rejected;
      ++plan.exact_field_evaluations_avoided;
      return (*interval)[0]>0.0?SurfaceRelation::outside:SurfaceRelation::inside;
    }
    return classify_tetrahedron_cached(
        mesh,owner,sphere,{},&plan.exact_field_evaluations);
  };
  const auto consider_split=[&](TetId owner,std::optional<double> known_diameter=std::nullopt){
    if(tet_depth(owner)+increment>maximum_depth){++plan.depth_rejections;return;}
    double diameter{};
    if(known_diameter)diameter=*known_diameter;
    else{
      ++plan.projection_evaluations;
      diameter=projected_tetrahedron_diameter(mesh,owner,prepared_camera);
    }
    if(diameter<=split_threshold)return;
    if(classify_owner(owner)==SurfaceRelation::intersecting)
      splits.push_back({owner,diameter/split_threshold});
  };
  if(configuration.candidate_traversal==CandidateTraversal::active_cut_scan){
    for(const TetId owner:logical)consider_split(owner);
  }else if(configuration.candidate_traversal==CandidateTraversal::spatial_runs){
    for(const auto& run:summaries.spatial_runs){
      ++plan.spatial_run_bound_tests;
      if(run.field_minimum>0.0||run.field_maximum<0.0){
        ++plan.field_subtrees_rejected;
        plan.exact_field_evaluations_avoided+=run.count;
        continue;
      }
      plan.spatial_run_candidates+=run.count;
      for(std::size_t index=run.begin;index<run.begin+run.count;++index)
        consider_split(logical[index]);
    }
  }else{
    const auto resident=[&](TetId owner){
      const auto depth=tet_depth(owner);
      if(depth>=mesh.layers().size())return false;
      const auto& records=mesh.layers()[depth].tetrahedra;
      const auto found=std::lower_bound(records.begin(),records.end(),owner,
          [](const Tetrahedron& record,TetId address){return record.address<address;});
      return found!=records.end()&&found->address==owner&&
          found->transition_parent==invalid_tet;
    };
    std::function<void(TetId)> visit=[&](TetId owner){
      ++plan.hierarchy_nodes_visited;
      ++plan.projection_evaluations;
      const double diameter=projected_tetrahedron_diameter(
          mesh,owner,prepared_camera);
      if(diameter==0.0){
        ++plan.frustum_subtrees_rejected;
        ++plan.exact_field_evaluations_avoided;
        return;
      }
      if(const auto interval=summary_interval(owner);
         interval&&((*interval)[0]>0.0||(*interval)[1]<0.0)){
        ++plan.field_subtrees_rejected;
        ++plan.exact_field_evaluations_avoided;
        return;
      }
      if(diameter<=split_threshold){
        ++plan.projected_subtrees_rejected;
        ++plan.exact_field_evaluations_avoided;
        return;
      }
      if(std::binary_search(logical.begin(),logical.end(),owner)){
        consider_split(owner,diameter);
        return;
      }
      for(std::uint32_t child=0;child<8U;++child){
        const TetId address=make_tet_id(
            tet_root(owner),(tet_path(owner)<<3U)|static_cast<TetId>(child));
        if(resident(address))visit(address);
      }
    };
    for(const auto& root:mesh.layers().front().tetrahedra)
      if(root.transition_parent==invalid_tet)visit(root.address);
  }
  std::sort(splits.begin(),splits.end(),[](const Candidate& left,const Candidate& right){
    if(left.priority!=right.priority)return left.priority>right.priority;
    return left.address<right.address;
  });
  plan.classification_ms=elapsed_ms(plan_start);
  plan.requested_splits=splits.size();

  // A split frontier takes precedence for a transaction. This keeps commit
  // atomic without copying the hierarchy and prevents conformity closure from
  // invalidating a simultaneous merge family. The next update can coarsen.
  if(!splits.empty()&&!has_overdepth){
    remember_stationary_result(false);
    if(configuration.lod_update==LodUpdateStrategy::saturated_clusters){
      // A saturated red cluster is one complete active sibling family.  At
      // depth zero the six cube roots form the initial cluster.  The maximum
      // member error is conservative: once one member requires refinement,
      // the entire cluster is emitted atomically.  This selection does not
      // inspect or iteratively expand through neighbouring tetrahedra.
      struct ClusterMember { TetId cluster{},owner{}; };
      struct Cluster { TetId key{};std::size_t begin{},end{};double priority{}; };
      std::vector<ClusterMember> members;
      members.reserve(logical.size());
      for(const TetId owner:logical){
        const TetId key=tet_depth(owner)>=3U
            ?make_tet_id(tet_root(owner),tet_path(owner)>>3U):invalid_tet;
        members.push_back({key,owner});
      }
      std::sort(members.begin(),members.end(),[](const auto& left,const auto& right){
        if(left.cluster!=right.cluster)return left.cluster<right.cluster;
        return left.owner<right.owner;
      });
      std::vector<Candidate> split_by_address=splits;
      std::sort(split_by_address.begin(),split_by_address.end(),
                [](const auto& left,const auto& right){return left.address<right.address;});
      std::vector<Cluster> clusters;
      for(std::size_t begin=0;begin<members.size();){
        std::size_t end=begin+1U;
        while(end<members.size()&&members[end].cluster==members[begin].cluster)++end;
        // A non-root red cluster is usable only while all eight siblings are
        // leaves.  Mixed-depth cuts retain their smaller complete clusters.
        const bool complete=members[begin].cluster==invalid_tet||end-begin==8U;
        double priority{};
        if(complete){
          for(std::size_t index=begin;index<end;++index){
            const auto found=std::lower_bound(
                split_by_address.begin(),split_by_address.end(),members[index].owner,
                [](const Candidate& candidate,TetId owner){return candidate.address<owner;});
            if(found!=split_by_address.end()&&found->address==members[index].owner)
              priority=std::max(priority,found->priority);
          }
        }
        if(priority>0.0)clusters.push_back({members[begin].cluster,begin,end,priority});
        begin=end;
      }
      std::sort(clusters.begin(),clusters.end(),[](const auto& left,const auto& right){
        if(left.priority!=right.priority)return left.priority>right.priority;
        return left.key<right.key;
      });
      plan.requested_splits=0U;
      for(const auto& cluster:clusters){
        const auto count=cluster.end-cluster.begin;
        plan.requested_splits+=count;
        if(plan.commands.size()+count>configuration.operation_budget){
          plan.over_budget=true;
          continue;
        }
        for(std::size_t index=cluster.begin;index<cluster.end;++index)
          plan.commands.push_back({members[index].owner,AdaptationCommandKind::split});
      }
      plan.planned_splits=plan.commands.size();
    }else{
      plan.over_budget=splits.size()>configuration.operation_budget;
      const auto count=std::min<std::size_t>(splits.size(),configuration.operation_budget);
      plan.commands.reserve(count);
      for(std::size_t index=0;index<count;++index)
        plan.commands.push_back({splits[index].address,AdaptationCommandKind::split});
      plan.planned_splits=count;
    }
    plan.family_resolution_ms=elapsed_ms(plan_start)-plan.classification_ms;
    order_mark_pass_fine_to_coarse(plan.commands);
    apply_scheduler(logical);
    pack_transaction_frontier(summaries,mesh.layers().size(),logical,plan.commands);
    if(planning_cache){
      summaries.has_split_pose=true;
      summaries.split_pose_field_revision=field_revision;
      summaries.split_pose_surface=sphere;
      summaries.split_pose_camera=camera;
      summaries.split_pose_configuration=configuration;
      summaries.split_pose_pixel_threshold=pixel_threshold;
      summaries.split_pose_maximum_depth=maximum_depth;
    }
    return plan;
  }

  if(split_origin_matches()&&!has_overdepth&&
     !summaries.pose_merge_pending){
    remember_stationary_result(true);
    pack_transaction_frontier(summaries,mesh.layers().size(),logical,{});
    return plan;
  }

  if(mesh.subdivision_method()!=SubdivisionMethod::bcc_red_green)return plan;
  std::vector<TetId> possible_parents;
  possible_parents.reserve(logical.size());
  for(const TetId owner:logical)if(tet_depth(owner)>=3U)
    possible_parents.push_back(make_tet_id(tet_root(owner),tet_path(owner)>>3U));
  std::sort(possible_parents.begin(),possible_parents.end());
  possible_parents.erase(std::unique(possible_parents.begin(),possible_parents.end()),
                         possible_parents.end());

  std::vector<Candidate> merges;
  const double merge_threshold=pixel_threshold*configuration.merge_hysteresis;
  for(const TetId parent:possible_parents){
    if(mesh.has_pinned_descendant(parent))continue;
    bool complete=true;
    for(std::uint32_t child=0;child<8U;++child){
      const TetId address=make_tet_id(tet_root(parent),
          (tet_path(parent)<<3U)|static_cast<TetId>(child));
      complete&=std::binary_search(logical.begin(),logical.end(),address);
    }
    if(!complete)continue;
    ++plan.projection_evaluations;
    const double diameter=projected_tetrahedron_diameter(
        mesh,parent,prepared_camera);
    const bool forced_depth=tet_depth(parent)+increment>maximum_depth;
    const bool size_merge=diameter<merge_threshold;
    if(forced_depth||size_merge){
      const double priority=forced_depth
          ?2.0e6+static_cast<double>(tet_depth(parent))
          :merge_threshold/std::max(diameter,1.0e-12);
      merges.push_back({parent,priority});
    }
  }
  std::sort(merges.begin(),merges.end(),[](const Candidate& left,const Candidate& right){
    if(left.priority!=right.priority)return left.priority>right.priority;
    return left.address<right.address;
  });
  plan.requested_merges=merges.size();
  const double classification_end=elapsed_ms(plan_start);
  plan.classification_ms=classification_end;
  // BCC midpoint ownership is shared by a same-generation closure cluster.
  // Test and commit complete depth bands instead of isolated parents: a lone
  // family may be illegal even though its conforming peer band is coarsenable.
  std::vector<unsigned int> depths;
  for(const auto& candidate:merges)depths.push_back(tet_depth(candidate.address));
  std::sort(depths.begin(),depths.end(),std::greater<>{});
  depths.erase(std::unique(depths.begin(),depths.end()),depths.end());
  std::vector<TetId> accepted;
  for(const unsigned int depth:depths){
    std::vector<TetId> band;
    for(const auto& candidate:merges)
      if(tet_depth(candidate.address)==depth)band.push_back(candidate.address);
    std::sort(band.begin(),band.end());
    if(band.size()>configuration.operation_budget){
      band.resize(configuration.operation_budget);
      plan.over_budget=true;
    }
    // A complete depth band may contain a small number of newly exposed
    // parents whose six-edge transition mask would require another red split.
    // Remove exactly those blockers and retry the remaining band; rejecting
    // every unrelated family made detailed cuts permanently unable to merge.
    if(!planning_cache){
      if(mesh.can_coarsen_selected_red(band))accepted=std::move(band);
      else plan.conformity_rejections+=band.size();
      if(!accepted.empty())break;
      continue;
    }
    while(!band.empty()){
      std::vector<TetId> blocked;
      if(mesh.can_coarsen_selected_red(band,&blocked)){
        accepted=std::move(band);
        break;
      }
      if(blocked.empty())break;
      plan.conformity_rejections+=blocked.size();
      std::vector<TetId> filtered;
      filtered.reserve(band.size());
      std::set_difference(band.begin(),band.end(),blocked.begin(),blocked.end(),
                          std::back_inserter(filtered));
      band=std::move(filtered);
    }
    if(!accepted.empty())break;
  }
  plan.commands.reserve(accepted.size());
  for(const TetId parent:accepted)
    plan.commands.push_back({parent,AdaptationCommandKind::merge});
  plan.planned_merges=accepted.size();
  plan.family_resolution_ms=elapsed_ms(plan_start)-classification_end;
  remember_stationary_result(plan.commands.empty());
  if(planning_cache&&plan.commands.empty())summaries.pose_merge_pending=false;
  order_mark_pass_fine_to_coarse(plan.commands);
  apply_scheduler(logical);
  pack_transaction_frontier(summaries,mesh.layers().size(),logical,plan.commands);
  return plan;
}

AdaptationCommitResult commit_adaptation(
    TetMesh& mesh,const AdaptationPlan& plan,
    const AdaptationConfiguration& current_configuration,
    std::uint64_t current_field_revision) {
  AdaptationCommitResult result;
  result.resulting_revision=mesh.revision();
  if(plan.base_revision!=mesh.revision()||
     plan.field_revision!=current_field_revision||
     plan.configuration!=current_configuration){
    result.status=AdaptationCommitStatus::stale_plan;
    return result;
  }
  if(!plan.supported){
    result.status=AdaptationCommitStatus::rejected;
    return result;
  }
  if(plan.over_budget&&plan.commands.empty()){
    result.status=AdaptationCommitStatus::rejected;
    return result;
  }
  // Planning writes marks fine-to-coarse. Once closure is stable, commit reads
  // the immutable command frontier coarse-to-fine so parents always precede
  // descendants in the mutation phase.
  auto commit_commands=plan.commands;
  std::sort(commit_commands.begin(),commit_commands.end(),[](const auto& first,
                                                              const auto& second){
    const auto first_depth=tet_depth(first.logical_owner);
    const auto second_depth=tet_depth(second.logical_owner);
    if(first_depth!=second_depth)return first_depth<second_depth;
    return first.logical_owner<second.logical_owner;
  });
  std::vector<TetId> splits,merges;
  for(const auto& command:commit_commands){
    if(command.kind==AdaptationCommandKind::split)splits.push_back(command.logical_owner);
    else if(command.kind==AdaptationCommandKind::merge)merges.push_back(command.logical_owner);
  }
  if(!splits.empty()&&!merges.empty()){
    result.status=AdaptationCommitStatus::rejected;
    return result;
  }
  const auto logical=mesh.logical_cut();
  result.replay.configuration=current_configuration;
  result.replay.field_revision=current_field_revision;
  result.replay.source_owner_hash=logical_owner_hash(logical.owners);
  result.replay.target_owner_hash=result.replay.source_owner_hash;
  if(splits.empty()&&merges.empty())return result;

  if(!splits.empty()){
    for(const TetId owner:splits)
      if(!std::binary_search(logical.owners.begin(),logical.owners.end(),owner)){
        result.status=AdaptationCommitStatus::rejected;
        return result;
      }
    BccClosureMode closure_mode=BccClosureMode::sparse_frontier;
    if(current_configuration.closure_execution==ClosureExecution::dense_level_sweep)
      closure_mode=BccClosureMode::dense_level_sweep;
    else if(current_configuration.closure_execution==ClosureExecution::hybrid)
      closure_mode=BccClosureMode::hybrid;
    if(!mesh.commit_planned_red_refinement(
           splits,closure_mode,current_configuration.hybrid_frontier_ratio)){
      result.status=AdaptationCommitStatus::rejected;
      return result;
    }
  }else{
    if(mesh.subdivision_method()!=SubdivisionMethod::bcc_red_green){
      result.status=AdaptationCommitStatus::rejected;
      return result;
    }
    if(!mesh.coarsen_selected_red(merges))return result;
  }
  const auto target=mesh.logical_cut();
  result.replay.target_owner_hash=logical_owner_hash(target.owners);
  std::vector<TetId> removed,added;
  removed.reserve(logical.owners.size()/8U+16U);
  added.reserve(target.owners.size()/8U+16U);
  std::set_difference(logical.owners.begin(),logical.owners.end(),
                      target.owners.begin(),target.owners.end(),
                      std::back_inserter(removed));
  std::set_difference(target.owners.begin(),target.owners.end(),
                      logical.owners.begin(),logical.owners.end(),
                      std::back_inserter(added));
  const auto has_family=[](std::span<const TetId> owners,TetId parent){
    for(std::uint32_t child=0;child<8U;++child){
      const TetId address=make_tet_id(
          tet_root(parent),(tet_path(parent)<<3U)|static_cast<TetId>(child));
      if(!std::binary_search(owners.begin(),owners.end(),address))return false;
    }
    return true;
  };
  for(const TetId owner:removed)
    if(has_family(added,owner))
      result.replay.forward_commands.push_back(
          {owner,AdaptationCommandKind::split});
  for(const TetId owner:added)
    if(has_family(removed,owner))
      result.replay.forward_commands.push_back(
          {owner,AdaptationCommandKind::merge});
  std::sort(result.replay.forward_commands.begin(),result.replay.forward_commands.end(),
            [](const auto& left,const auto& right){
              if(left.logical_owner!=right.logical_owner)
                return left.logical_owner<right.logical_owner;
              return left.kind<right.kind;
            });
  result.replay.reverse_commands.reserve(result.replay.forward_commands.size());
  for(auto command:result.replay.forward_commands){
    command.kind=command.kind==AdaptationCommandKind::split
        ?AdaptationCommandKind::merge:AdaptationCommandKind::split;
    result.replay.reverse_commands.push_back(command);
  }
  result.accepted_splits=static_cast<std::size_t>(std::ranges::count_if(
      result.replay.forward_commands,[](const auto& command){
        return command.kind==AdaptationCommandKind::split;
      }));
  result.accepted_merges=result.replay.forward_commands.size()-result.accepted_splits;
  result.status=AdaptationCommitStatus::committed;
  result.resulting_revision=mesh.revision();
  result.bcc_metrics=mesh.last_bcc_update_metrics();
  return result;
}

AdaptationCommitResult replay_adaptation(
    TetMesh& mesh,const AdaptationReplayRecord& record,bool reverse,
    const AdaptationConfiguration& current_configuration,
    std::uint64_t current_field_revision) {
  AdaptationCommitResult result;
  result.resulting_revision=mesh.revision();
  if(record.schema_version!=adaptation_replay_schema_version||
     record.configuration!=current_configuration||
     record.field_revision!=current_field_revision){
    result.status=AdaptationCommitStatus::rejected;
    return result;
  }
  const auto logical=mesh.logical_cut();
  const std::uint64_t expected=reverse?record.target_owner_hash:record.source_owner_hash;
  if(logical_owner_hash(logical.owners)!=expected){
    result.status=AdaptationCommitStatus::stale_plan;
    return result;
  }
  AdaptationPlan plan;
  plan.base_revision=mesh.revision();
  plan.field_revision=current_field_revision;
  plan.configuration=current_configuration;
  plan.supported=implemented(current_configuration)&&
      (current_configuration.lod_update==LodUpdateStrategy::transactional_active_cut||
       current_configuration.lod_update==LodUpdateStrategy::saturated_clusters)&&
      mesh.subdivision_method()==SubdivisionMethod::bcc_red_green;
  plan.commands=reverse?record.reverse_commands:record.forward_commands;
  for(const auto& command:plan.commands){
    plan.planned_splits+=command.kind==AdaptationCommandKind::split?1U:0U;
    plan.planned_merges+=command.kind==AdaptationCommandKind::merge?1U:0U;
  }
  result=commit_adaptation(mesh,plan,current_configuration,current_field_revision);
  if(result.status==AdaptationCommitStatus::committed){
    const auto final_hash=logical_owner_hash(mesh.logical_cut().owners);
    const std::uint64_t wanted=reverse?record.source_owner_hash:record.target_owner_hash;
    if(final_hash!=wanted)throw std::logic_error("adaptation replay hash mismatch");
  }
  return result;
}

AdaptationCommitResult adapt_to_surface(TetMesh& mesh,const Sphere& sphere,
                                        const Camera& camera,double pixel_threshold,
                                        unsigned int maximum_depth,
                                        const AdaptationConfiguration& configuration,
                                        std::uint64_t field_revision,
                                        AdaptationPlanningCache* planning_cache) {
  const auto plan=plan_adaptation(mesh,sphere,camera,pixel_threshold,
                                  maximum_depth,configuration,
                                  field_revision,planning_cache);
  auto result=commit_adaptation(mesh,plan,configuration,field_revision);
  if(planning_cache&&result.status==AdaptationCommitStatus::no_change&&
     !plan.commands.empty())planning_cache->pose_merge_pending=false;
  return result;
}

bool update_fixed_field_surface_hierarchy(
    FixedFieldSurfaceHierarchy& hierarchy,const TetMesh& mesh,
    const Sphere& sphere,std::uint64_t field_revision) {
  if(hierarchy.field_revision==field_revision&&
     hierarchy.source_resident_revision==mesh.resident_revision())return false;
  hierarchy.layers.clear();
  hierarchy.layers.resize(mesh.layers().size());
  hierarchy.relevant_clusters=0;
  hierarchy.minimal_clusters=0;

  std::vector<std::vector<TetId>> intersecting(mesh.layers().size());
  for(std::size_t depth=0;depth<mesh.layers().size();++depth){
    for(const auto& record:mesh.layers()[depth].tetrahedra){
      if(record.transition_parent!=invalid_tet)continue;
      if(classify_tetrahedron(mesh,record.address,sphere)==SurfaceRelation::intersecting)
        intersecting[depth].push_back(record.address);
    }
  }
  const auto is_intersecting=[&](TetId address){
    const auto depth=tet_depth(address);
    return depth<intersecting.size()&&std::binary_search(
        intersecting[depth].begin(),intersecting[depth].end(),address);
  };
  const auto has_intersecting_descendant=[&](TetId address){
    const auto depth=tet_depth(address);
    for(std::size_t child_depth=depth+3U;child_depth<intersecting.size();child_depth+=3U){
      const auto shift=child_depth-depth;
      const auto found=std::lower_bound(intersecting[child_depth].begin(),
          intersecting[child_depth].end(),address,[&](TetId child,TetId ancestor){
            if(tet_root(child)!=tet_root(ancestor))return tet_root(child)<tet_root(ancestor);
            return (tet_path(child)>>shift)<tet_path(ancestor);
          });
      if(found!=intersecting[child_depth].end()&&tet_root(*found)==tet_root(address)&&
         (tet_path(*found)>>shift)==tet_path(address))return true;
    }
    return false;
  };

  for(std::size_t depth=0;depth<intersecting.size();++depth){
    auto& output=hierarchy.layers[depth];
    output.relevant_addresses=intersecting[depth];
    output.active_words.assign((intersecting[depth].size()+63U)/64U,0U);
    output.topology_creation_words.assign((intersecting[depth].size()+63U)/64U,0U);
    for(std::size_t index=0;index<intersecting[depth].size();++index){
      const TetId address=intersecting[depth][index];
      std::size_t child_count{};
      if(depth+3U<intersecting.size()){
        for(std::uint32_t child=0;child<8U;++child){
          const TetId child_address=make_tet_id(
              tet_root(address),(tet_path(address)<<3U)|static_cast<TetId>(child));
          child_count+=is_intersecting(child_address)?1U:0U;
        }
      }
      const bool active=child_count==0U&&!has_intersecting_descendant(address);
      const bool topology=tet_depth(address)==0U||child_count>1U;
      if(active)output.active_words[index/64U]|=std::uint64_t{1}<<(index%64U);
      if(topology)output.topology_creation_words[index/64U]|=
          std::uint64_t{1}<<(index%64U);
      if(active||topology)output.minimal_addresses.push_back(address);
    }
    hierarchy.relevant_clusters+=output.relevant_addresses.size();
    hierarchy.minimal_clusters+=output.minimal_addresses.size();
  }
  hierarchy.retained_bytes=0;
  for(const auto& layer:hierarchy.layers){
    hierarchy.retained_bytes+=
        (layer.relevant_addresses.capacity()+layer.minimal_addresses.capacity())*sizeof(TetId)+
        (layer.active_words.capacity()+layer.topology_creation_words.capacity())*
            sizeof(std::uint64_t);
  }
  hierarchy.field_revision=field_revision;
  hierarchy.source_resident_revision=mesh.resident_revision();
  ++hierarchy.rebuild_count;
  return true;
}

std::vector<TetId> select_fixed_field_surface_cut(
    const FixedFieldSurfaceHierarchy& hierarchy,const TetMesh& mesh,
    const Camera& camera,double pixel_threshold,unsigned int maximum_depth,
    LodUpdateStrategy strategy) {
  if(strategy!=LodUpdateStrategy::relevant_surface_hierarchy&&
     strategy!=LodUpdateStrategy::minimal_surface_hierarchy)
    throw std::invalid_argument("LOD strategy is not a fixed-field surface hierarchy");
  std::vector<TetId> candidates;
  for(const auto& layer:hierarchy.layers){
    const auto& addresses=strategy==LodUpdateStrategy::relevant_surface_hierarchy
        ?layer.relevant_addresses:layer.minimal_addresses;
    candidates.insert(candidates.end(),addresses.begin(),addresses.end());
  }
  std::sort(candidates.begin(),candidates.end());
  std::vector<TetId> selected;
  selected.reserve(candidates.size());
  for(const TetId address:candidates){
    if(tet_depth(address)>maximum_depth)continue;
    bool covered=false;
    TetId ancestor=address;
    while(tet_depth(ancestor)>=3U){
      ancestor=make_tet_id(tet_root(ancestor),tet_path(ancestor)>>3U);
      if(std::binary_search(selected.begin(),selected.end(),ancestor)){
        covered=true;break;
      }
    }
    if(covered)continue;
    const double diameter=projected_tetrahedron_diameter(mesh,address,camera);
    const bool terminal=diameter<=pixel_threshold||tet_depth(address)==maximum_depth;
    bool has_candidate_child=false;
    if(!terminal){
      for(std::uint32_t child=0;child<8U&&!has_candidate_child;++child){
        const TetId child_address=make_tet_id(
            tet_root(address),(tet_path(address)<<3U)|static_cast<TetId>(child));
        has_candidate_child=std::binary_search(candidates.begin(),candidates.end(),child_address);
      }
    }
    if(terminal||!has_candidate_child){
      const auto position=std::lower_bound(selected.begin(),selected.end(),address);
      selected.insert(position,address);
    }
  }
  return selected;
}

std::vector<TetId> query_relevant_surface_hierarchy(
    const FixedFieldSurfaceHierarchy& hierarchy,const TetMesh& mesh,
    Vec3 minimum,Vec3 maximum) {
  std::vector<TetId> result;
  for(const auto& layer:hierarchy.layers){
    for(const TetId address:layer.relevant_addresses){
      const auto& record=mesh.tetrahedron(address);
      Vec3 cell_minimum{std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::infinity()};
      Vec3 cell_maximum{-std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity()};
      for(const VertexId vertex:record.vertices){
        const auto& point=mesh.vertices()[vertex];
        cell_minimum.x=std::min(cell_minimum.x,point.x);
        cell_minimum.y=std::min(cell_minimum.y,point.y);
        cell_minimum.z=std::min(cell_minimum.z,point.z);
        cell_maximum.x=std::max(cell_maximum.x,point.x);
        cell_maximum.y=std::max(cell_maximum.y,point.y);
        cell_maximum.z=std::max(cell_maximum.z,point.z);
      }
      if(cell_maximum.x>=minimum.x&&cell_minimum.x<=maximum.x&&
         cell_maximum.y>=minimum.y&&cell_minimum.y<=maximum.y&&
         cell_maximum.z>=minimum.z&&cell_minimum.z<=maximum.z)
        result.push_back(address);
    }
  }
  std::sort(result.begin(),result.end());
  return result;
}

bool update_preorder_surface_hierarchy(
    PreorderSurfaceHierarchy& preorder,
    const FixedFieldSurfaceHierarchy& fixed_field) {
  if(preorder.field_revision==fixed_field.field_revision&&
     preorder.source_resident_revision==fixed_field.source_resident_revision)return false;
  preorder.addresses.clear();
  preorder.descendant_counts.clear();
  preorder.child_indices.clear();
  preorder.roots.clear();
  std::vector<TetId> relevant;
  for(const auto& layer:fixed_field.layers)
    relevant.insert(relevant.end(),layer.relevant_addresses.begin(),
                    layer.relevant_addresses.end());
  std::sort(relevant.begin(),relevant.end());
  const auto present=[&](TetId address){
    return std::binary_search(relevant.begin(),relevant.end(),address);
  };
  std::function<std::uint32_t(TetId)> append=[&](TetId address){
    const auto index=static_cast<std::uint32_t>(preorder.addresses.size());
    preorder.addresses.push_back(address);
    preorder.descendant_counts.push_back(0U);
    preorder.child_indices.resize(preorder.child_indices.size()+8U,
                                  std::numeric_limits<std::uint32_t>::max());
    for(std::uint32_t child=0;child<8U;++child){
      const TetId child_address=make_tet_id(
          tet_root(address),(tet_path(address)<<3U)|static_cast<TetId>(child));
      if(!present(child_address))continue;
      preorder.child_indices[static_cast<std::size_t>(index)*8U+child]=
          append(child_address);
    }
    preorder.descendant_counts[index]=
        static_cast<std::uint32_t>(preorder.addresses.size()-index-1U);
    return index;
  };
  if(!fixed_field.layers.empty()){
    for(const TetId root:fixed_field.layers.front().relevant_addresses)
      preorder.roots.push_back(append(root));
  }
  preorder.field_revision=fixed_field.field_revision;
  preorder.source_resident_revision=fixed_field.source_resident_revision;
  preorder.retained_bytes=preorder.addresses.capacity()*sizeof(TetId)+
      (preorder.descendant_counts.capacity()+preorder.child_indices.capacity()+
       preorder.roots.capacity())*sizeof(std::uint32_t);
  ++preorder.rebuild_count;
  return true;
}

PreorderRenderMetrics render_preorder_surface(
    const PreorderSurfaceHierarchy& preorder,const TetMesh& mesh,
    const Sphere& sphere,const Camera& camera,double pixel_threshold,
    unsigned int maximum_depth,std::vector<Triangle>& retained_triangles) {
  const auto start=std::chrono::steady_clock::now();
  PreorderRenderMetrics metrics;
  retained_triangles.clear();
  std::function<void(std::uint32_t)> visit=[&](std::uint32_t index){
    ++metrics.nodes_visited;
    const TetId address=preorder.addresses[index];
    bool has_child=false;
    for(std::size_t child=0;child<8U;++child)
      has_child|=preorder.child_indices[static_cast<std::size_t>(index)*8U+child]!=
          std::numeric_limits<std::uint32_t>::max();
    const bool stop=!has_child||tet_depth(address)>=maximum_depth||
        projected_tetrahedron_diameter(mesh,address,camera)<=pixel_threshold;
    if(stop){
      ++metrics.selected_nodes;
      const std::array<TetId,1> selected{{address}};
      auto triangles=extract_isosurface(mesh,sphere,selected);
      retained_triangles.insert(retained_triangles.end(),triangles.begin(),triangles.end());
      return;
    }
    for(std::size_t child=0;child<8U;++child){
      const auto child_index=
          preorder.child_indices[static_cast<std::size_t>(index)*8U+child];
      if(child_index!=std::numeric_limits<std::uint32_t>::max())visit(child_index);
    }
  };
  for(const auto root:preorder.roots)visit(root);
  metrics.generated_triangles=retained_triangles.size();
  metrics.traversal_ms=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-start).count();
  return metrics;
}

std::vector<Triangle> extract_isosurface(
    const TetMesh& mesh,const Sphere& sphere,std::span<const TetId> tetrahedra) {
  std::vector<Triangle> triangles;
  constexpr std::array<std::array<int, 2>, 6> edges{{{{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};
  const auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
  const auto dot = [](Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  for (const TetId id : tetrahedra) {
    const auto& tet = mesh.tetrahedron(id).vertices;
    std::array<Vec3, 4> points{};
    std::array<double, 4> distances{};
    for (std::size_t i = 0; i < points.size(); ++i) {
      points[i] = mesh.vertices().at(tet[i]);
      distances[i] = sphere.signed_distance(points[i]);
    }
    std::vector<Vec3> crossings;
    for (const auto& edge : edges) {
      const std::size_t first = static_cast<std::size_t>(edge[0]), second = static_cast<std::size_t>(edge[1]);
      if ((distances[first] < 0.0) == (distances[second] < 0.0)) continue;
      crossings.push_back(sphere.edge_intersection(points[first],points[second]));
    }
    if (crossings.size() < 3) continue;
    Vec3 centre{};
    for (const Vec3 point : crossings) centre = centre + point;
    centre = centre / static_cast<double>(crossings.size());
    const Vec3 normal=sphere.normal(centre);
    const Vec3 reference = std::abs(normal.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0};
    const Vec3 axis_u = cross(reference, normal);
    const Vec3 axis_v = cross(normal, axis_u);
    std::sort(crossings.begin(), crossings.end(), [&](Vec3 left, Vec3 right) {
      const Vec3 left_offset = left - centre, right_offset = right - centre;
      return std::atan2(dot(left_offset, axis_v), dot(left_offset, axis_u)) < std::atan2(dot(right_offset, axis_v), dot(right_offset, axis_u));
    });
    for (std::size_t index = 1; index + 1 < crossings.size(); ++index) triangles.push_back({crossings[0], crossings[index], crossings[index + 1]});
  }
  return triangles;

  constexpr std::array<std::array<int, 2>, 6> legacy_edges{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  const auto sphere_intersects_simplex = [&sphere](const std::array<Vec3, 4>& vertices) {
    const auto signed_volume = [](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
      return signed_six_volume(a, b, c, d);
    };
    const double total = signed_volume(vertices[0], vertices[1], vertices[2], vertices[3]);
    if (std::abs(total) > 1e-12) {
      const double w0 = signed_volume(sphere.centre, vertices[1], vertices[2], vertices[3]) / total;
      const double w1 = signed_volume(vertices[0], sphere.centre, vertices[2], vertices[3]) / total;
      const double w2 = signed_volume(vertices[0], vertices[1], sphere.centre, vertices[3]) / total;
      const double w3 = signed_volume(vertices[0], vertices[1], vertices[2], sphere.centre) / total;
      constexpr double epsilon = 1e-10;
      if (w0 >= -epsilon && w1 >= -epsilon && w2 >= -epsilon && w3 >= -epsilon) return true;
    }
    constexpr std::array<std::array<std::size_t, 3>, 4> faces{{{{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}}}};
    for (const auto& face : faces) {
      if (squared_distance_to_triangle(sphere.centre, vertices[face[0]], vertices[face[1]], vertices[face[2]]) <= sphere.radius * sphere.radius) return true;
    }
    return false;
  };
  const auto polygonize = [&sphere, &triangles, &legacy_edges, &sphere_intersects_simplex](auto&& self, const std::array<Vec3, 4>& vertices, unsigned int remaining_depth) -> void {
    std::vector<Vec3> crossings;
    std::array<double, 4> distances{};
    bool has_negative = false;
    bool has_positive = false;
    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
      distances[vertex] = sphere.signed_distance(vertices[vertex]);
      has_negative |= distances[vertex] < 0.0;
      has_positive |= distances[vertex] > 0.0;
    }
    if (!has_negative || !has_positive) {
      // If all four corners are inside, convexity guarantees that this whole
      // simplex is inside the sphere.  For an all-outside simplex, however,
      // the sphere can pass through a face interior without touching any edge.
      // Subdivide that case before declaring it empty.
      if (has_positive && remaining_depth > 0 && sphere_intersects_simplex(vertices)) {
        const Vec3 ab = (vertices[0] + vertices[1]) / 2.0, ac = (vertices[0] + vertices[2]) / 2.0;
        const Vec3 ad = (vertices[0] + vertices[3]) / 2.0, bc = (vertices[1] + vertices[2]) / 2.0;
        const Vec3 bd = (vertices[1] + vertices[3]) / 2.0, cd = (vertices[2] + vertices[3]) / 2.0;
        const std::array<std::array<Vec3, 4>, 8> children{{
            {{vertices[0], ab, ac, ad}}, {{ab, vertices[1], bc, bd}},
            {{ac, bc, vertices[2], cd}}, {{ad, bd, cd, vertices[3]}},
            {{ab, ac, ad, cd}}, {{ab, ac, bc, cd}}, {{ab, ad, bd, cd}}, {{ab, bc, bd, cd}},
        }};
        for (const auto& child : children) self(self, child, remaining_depth - 1);
      }
      return;
    }
    for (const auto& edge : legacy_edges) {
      const auto first = static_cast<std::size_t>(edge[0]);
      const auto second = static_cast<std::size_t>(edge[1]);
      const Vec3 a = vertices[first], b = vertices[second];
      const double da = distances[first], db = distances[second];
      if ((da < 0.0) == (db < 0.0)) continue;
      // The experiment's field is an analytic sphere, so intersect its
      // quadratic exactly instead of treating the endpoint SDF values as a
      // linear field.  This keeps the displayed/extracted surface on the
      // declared sphere even in coarse leaves.
      const Vec3 offset{a.x - sphere.centre.x, a.y - sphere.centre.y, a.z - sphere.centre.z};
      const Vec3 direction{b.x - a.x, b.y - a.y, b.z - a.z};
      const double quadratic_a = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
      const double quadratic_b = 2.0 * (offset.x * direction.x + offset.y * direction.y + offset.z * direction.z);
      const double quadratic_c = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z - sphere.radius * sphere.radius;
      const double discriminant = std::max(0.0, quadratic_b * quadratic_b - 4.0 * quadratic_a * quadratic_c);
      const double root0 = (-quadratic_b - std::sqrt(discriminant)) / (2.0 * quadratic_a);
      const double root1 = (-quadratic_b + std::sqrt(discriminant)) / (2.0 * quadratic_a);
      const double t = root0 >= 0.0 && root0 <= 1.0 ? root0 : root1;
      const Vec3 crossing{a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t};
      const auto same_point = [&crossing](const Vec3& existing) {
        const Vec3 delta = existing - crossing;
        return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z < 1e-20;
      };
      if (std::ranges::none_of(crossings, same_point)) crossings.push_back(crossing);
    }
    if (crossings.size() == 3) triangles.push_back({crossings[0], crossings[1], crossings[2]});
    if (crossings.size() == 4) {
      triangles.push_back({crossings[0], crossings[1], crossings[2]});
      triangles.push_back({crossings[0], crossings[2], crossings[3]});
    }
  };
  for (const TetId id : mesh.conforming_volume().addresses()) {
    const auto& tet = mesh.tetrahedron(id);
    const std::array<Vec3, 4> vertices{{mesh.vertices().at(tet.vertices[0]), mesh.vertices().at(tet.vertices[1]), mesh.vertices().at(tet.vertices[2]), mesh.vertices().at(tet.vertices[3])}};
    polygonize(polygonize, vertices, 3);
  }
  return triangles;
}

std::vector<Triangle> extract_isosurface(const TetMesh& mesh, const Sphere& sphere) {
  return extract_isosurface(mesh,sphere,mesh.conforming_volume().addresses());
}

std::vector<Triangle> extract_dual_contour(const TetMesh& mesh, const Sphere& sphere) {
  constexpr std::array<std::array<std::size_t, 2>, 6> tet_edges{{
      {{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};
  const auto dot = [](Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  const auto cross = [](Vec3 a, Vec3 b) {
    return Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
  };
  const auto length_squared = [&dot](Vec3 value) { return dot(value, value); };
  const auto normalize = [&length_squared](Vec3 value) {
    const double length = std::sqrt(length_squared(value));
    return length > 1e-15 ? value / length : Vec3{};
  };
  const auto packed_edge = [](VertexId first, VertexId second) {
    if (second < first) std::swap(first, second);
    return (static_cast<std::uint64_t>(first) << 32U) | static_cast<std::uint64_t>(second);
  };
  const auto edge_crossing = [&sphere](Vec3 first, Vec3 second) {
    return sphere.edge_intersection(first,second);
  };

  struct EdgeIncident {
    std::uint64_t edge{};
    std::uint32_t cell{};
  };
  std::vector<Vec3> dual_vertices;
  std::vector<EdgeIncident> incidents;
  dual_vertices.reserve(mesh.conforming_volume().size()/4);
  incidents.reserve(mesh.conforming_volume().size());

  for (const TetId id : mesh.conforming_volume().addresses()) {
    const auto& tet = mesh.tetrahedron(id);
    std::array<Vec3, 4> points{};
    std::array<double, 4> distances{};
    for (std::size_t index = 0; index < 4; ++index) {
      points[index] = mesh.vertices()[tet.vertices[index]];
      distances[index] = sphere.signed_distance(points[index]);
    }

    std::array<Vec3, 6> crossings{};
    std::array<Vec3, 6> normals{};
    std::size_t crossing_count = 0;
    std::array<std::uint64_t, 6> crossed_edges{};
    std::size_t crossed_edge_count = 0;
    for (const auto edge : tet_edges) {
      const auto first = edge[0], second = edge[1];
      if ((distances[first] < 0.0) == (distances[second] < 0.0)) continue;
      const Vec3 crossing = edge_crossing(points[first], points[second]);
      crossings[crossing_count] = crossing;
      normals[crossing_count] = sphere.normal(crossing);
      ++crossing_count;
      crossed_edges[crossed_edge_count++] = packed_edge(tet.vertices[first], tet.vertices[second]);
    }
    if (crossing_count < 3) continue;

    Vec3 mass_point{};
    for (std::size_t sample = 0; sample < crossing_count; ++sample)
      mass_point = mass_point+crossings[sample];
    mass_point = mass_point/static_cast<double>(crossing_count);

    // Minimize the Hermite plane error. A small pull toward the mass point
    // regularizes the nearly parallel normals found in small smooth cells.
    double augmented[3][4]{};
    for (std::size_t sample = 0; sample < crossing_count; ++sample) {
      const double n[3]{normals[sample].x, normals[sample].y, normals[sample].z};
      const double plane = dot(normals[sample], crossings[sample]);
      for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column)
          augmented[row][column] += n[row]*n[column];
        augmented[row][3] += n[row]*plane;
      }
    }
    const double regularization = 1e-2*static_cast<double>(crossing_count);
    const double mass[3]{mass_point.x, mass_point.y, mass_point.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      augmented[axis][axis] += regularization;
      augmented[axis][3] += regularization*mass[axis];
    }
    bool solvable = true;
    for (std::size_t column = 0; column < 3; ++column) {
      std::size_t pivot = column;
      for (std::size_t row = column+1; row < 3; ++row)
        if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
      if (std::abs(augmented[pivot][column]) < 1e-12) {
        solvable = false;
        break;
      }
      if (pivot != column) for (std::size_t entry = column; entry < 4; ++entry)
        std::swap(augmented[pivot][entry], augmented[column][entry]);
      const double divisor = augmented[column][column];
      for (std::size_t entry = column; entry < 4; ++entry) augmented[column][entry] /= divisor;
      for (std::size_t row = 0; row < 3; ++row) {
        if (row == column) continue;
        const double factor = augmented[row][column];
        for (std::size_t entry = column; entry < 4; ++entry)
          augmented[row][entry] -= factor*augmented[column][entry];
      }
    }
    Vec3 dual = solvable ? Vec3{augmented[0][3], augmented[1][3], augmented[2][3]} : mass_point;
    if (!contains_point(mesh, tet, dual)) {
      // The cell owns its dual vertex. Constrain an outlying QEF solution by
      // walking back toward the crossing mass point, which is inside the
      // convex tetrahedron.
      double lower = 0.0, upper = 1.0;
      for (unsigned int iteration = 0; iteration < 48; ++iteration) {
        const double t = (lower+upper)*0.5;
        const Vec3 candidate{mass_point.x+t*(dual.x-mass_point.x),
                             mass_point.y+t*(dual.y-mass_point.y),
                             mass_point.z+t*(dual.z-mass_point.z)};
        if (contains_point(mesh, tet, candidate)) lower = t;
        else upper = t;
      }
      dual = {mass_point.x+lower*(dual.x-mass_point.x),
              mass_point.y+lower*(dual.y-mass_point.y),
              mass_point.z+lower*(dual.z-mass_point.z)};
    }
    Vec3 tet_centre{};
    for (const Vec3 point : points) tet_centre=tet_centre+point;
    tet_centre=tet_centre/4.0;
    // Dual topology assigns a distinct vertex to every cut cell. Keep it
    // strictly inside its owner so adjacent constrained QEF solutions cannot
    // collapse onto the same face and create zero-area dual polygons.
    constexpr double ownership_inset = 1e-4;
    dual={dual.x*(1.0-ownership_inset)+tet_centre.x*ownership_inset,
          dual.y*(1.0-ownership_inset)+tet_centre.y*ownership_inset,
          dual.z*(1.0-ownership_inset)+tet_centre.z*ownership_inset};

    const auto cell = static_cast<std::uint32_t>(dual_vertices.size());
    dual_vertices.push_back(dual);
    for (std::size_t edge = 0; edge < crossed_edge_count; ++edge)
      incidents.push_back({crossed_edges[edge], cell});
  }

  std::sort(incidents.begin(), incidents.end(), [](const EdgeIncident& first, const EdgeIncident& second) {
    return first.edge < second.edge || (first.edge == second.edge && first.cell < second.cell);
  });
  std::vector<Triangle> triangles;
  triangles.reserve(incidents.size()*2);
  std::vector<Vec3> polygon;
  polygon.reserve(16);
  for (std::size_t begin = 0; begin < incidents.size();) {
    std::size_t end = begin+1;
    while (end < incidents.size() && incidents[end].edge == incidents[begin].edge) ++end;
    if (end-begin < 3) {
      begin = end;
      continue;
    }

    const VertexId first_id = static_cast<VertexId>(incidents[begin].edge >> 32U);
    const VertexId second_id = static_cast<VertexId>(incidents[begin].edge);
    const Vec3 first = mesh.vertices()[first_id], second = mesh.vertices()[second_id];
    const Vec3 edge_point = edge_crossing(first, second);
    const Vec3 axis = normalize(second-first);
    const Vec3 reference = std::abs(axis.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0};
    const Vec3 basis_u = normalize(cross(reference, axis));
    const Vec3 basis_v = cross(axis, basis_u);
    polygon.clear();
    polygon.reserve(end-begin);
    for (std::size_t incident = begin; incident < end; ++incident)
      polygon.push_back(dual_vertices[incidents[incident].cell]);
    std::sort(polygon.begin(), polygon.end(), [&](Vec3 left, Vec3 right) {
      const Vec3 left_offset = left-edge_point, right_offset = right-edge_point;
      return std::atan2(dot(left_offset, basis_v), dot(left_offset, basis_u)) <
             std::atan2(dot(right_offset, basis_v), dot(right_offset, basis_u));
    });

    const Vec3 polygon_normal = cross(polygon[1]-polygon[0], polygon[2]-polygon[0]);
    if (dot(polygon_normal, sphere.normal(edge_point)) < 0.0)
      std::reverse(polygon.begin(), polygon.end());
    // A ring of constrained QEF vertices is not necessarily convex. A fan
    // from one ring vertex can cross outside the dual face and overlap other
    // faces. The exact field crossing on the primal edge lies in every local
    // edge wedge, so triangulating consecutive ring vertices through it keeps
    // every triangle local and exposes the complete surface wireframe.
    for (std::size_t index = 0; index < polygon.size(); ++index) {
      Triangle triangle{edge_point,polygon[index],polygon[(index+1)%polygon.size()]};
      const Vec3 triangle_normal=cross(triangle.b-triangle.a,triangle.c-triangle.a);
      const Vec3 triangle_centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                                 (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                                 (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
      if (dot(triangle_normal,sphere.normal(triangle_centre)) < 0.0)
        std::swap(triangle.b,triangle.c);
      if (length_squared(triangle_normal) > 1e-24) triangles.push_back(triangle);
    }
    begin = end;
  }
  return triangles;
}

}  // namespace tetra
