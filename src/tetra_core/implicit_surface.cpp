#include "tetra_core/implicit_surface.hpp"

#include <algorithm>
#include <cmath>
#include <span>

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
      // contributes the same slope.  Eight is a conservative derivative
      // bound for the smooth gradient-noise interpolation used above.
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
                                             std::span<const double> vertex_distances) {
  const auto& vertices = mesh.tetrahedron(tet).vertices;
  bool has_negative = false;
  bool has_positive = false;
  Vec3 centre{};
  for (const VertexId vertex : vertices) {
    const Vec3 point=mesh.vertices().at(vertex);
    const double distance=vertex_distances.empty()?
        sphere.signed_distance(point):vertex_distances[vertex];
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
  const double uncertainty=field_lipschitz_bound(sphere)*std::sqrt(radius_squared);
  if(centre_distance>uncertainty)return SurfaceRelation::outside;
  if(centre_distance<-uncertainty)return SurfaceRelation::inside;
  return SurfaceRelation::intersecting;
}

SurfaceRelation classify_tetrahedron(const TetMesh& mesh,TetId tet,const Sphere& sphere){
  return classify_tetrahedron_cached(mesh,tet,sphere,{});
}

double projected_tetrahedron_diameter(const TetMesh& mesh, TetId tet, const Camera& camera) {
  const auto& vertices = mesh.tetrahedron(tet).vertices;
  Vec3 centre{};
  for (const VertexId vertex : vertices) centre = centre + mesh.vertices().at(vertex);
  centre = centre / 4.0;
  double radius_squared = 0.0;
  for (const VertexId vertex : vertices) {
    const Vec3 delta = mesh.vertices().at(vertex) - centre;
    radius_squared = std::max(radius_squared, delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  }
  const Vec3 view = centre - camera.position;
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
  const Vec3 forward=normalize(camera.forward);
  const Vec3 right=normalize(cross(forward,camera.up));
  const Vec3 up=normalize(cross(right,forward));
  const double radius=std::sqrt(radius_squared);
  const double depth=dot(view,forward);
  if(depth+radius<=0.0)return 0.0;
  const double tangent=std::tan(camera.vertical_fov_radians*0.5);
  const double half_height=std::max(0.0,depth)*tangent;
  const double half_width=half_height*camera.aspect_ratio;
  if(std::abs(dot(view,right))>half_width+radius||
     std::abs(dot(view,up))>half_height+radius)return 0.0;
  if(depth<=radius)return camera.viewport_height_pixels;
  const double focal_length = (camera.viewport_height_pixels * 0.5) / std::tan(camera.vertical_fov_radians * 0.5);
  return (2.0*radius*focal_length)/depth;
}

std::vector<TetId> mark_oversized_intersections(const TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold) {
  std::vector<TetId> marked;
  for (const TetId id : mesh.active_leaves()) {
    if (classify_tetrahedron(mesh, id, sphere) == SurfaceRelation::intersecting &&
        projected_tetrahedron_diameter(mesh, id, camera) > pixel_threshold) marked.push_back(id);
  }
  return marked;
}

AdaptiveResult refine_to_sphere(TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold, unsigned int maximum_depth) {
  AdaptiveResult result;
  const unsigned int increment=subdivision_depth_increment(mesh.subdivision_method());
  std::vector<double> vertex_distances;
  while(true){
    const std::size_t previous_size=vertex_distances.size();
    vertex_distances.resize(mesh.vertices().size());
    for(std::size_t vertex=previous_size;vertex<vertex_distances.size();++vertex)
      vertex_distances[vertex]=sphere.signed_distance(mesh.vertices()[vertex]);
    std::vector<TetId> oversized;
    for(const TetId id:mesh.active_leaves())
      if(classify_tetrahedron_cached(mesh,id,sphere,vertex_distances)==
             SurfaceRelation::intersecting&&
         projected_tetrahedron_diameter(mesh,id,camera)>pixel_threshold)
        oversized.push_back(id);
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

std::vector<Triangle> extract_isosurface(const TetMesh& mesh, const Sphere& sphere) {
  std::vector<Triangle> triangles;
  constexpr std::array<std::array<int, 2>, 6> edges{{{{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};
  const auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
  const auto dot = [](Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  for (const TetId id : mesh.active_leaves()) {
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
  for (const TetId id : mesh.active_leaves()) {
    const auto& tet = mesh.tetrahedron(id);
    const std::array<Vec3, 4> vertices{{mesh.vertices().at(tet.vertices[0]), mesh.vertices().at(tet.vertices[1]), mesh.vertices().at(tet.vertices[2]), mesh.vertices().at(tet.vertices[3])}};
    polygonize(polygonize, vertices, 3);
  }
  return triangles;
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
  dual_vertices.reserve(mesh.active_leaves().size()/4);
  incidents.reserve(mesh.active_leaves().size());

  for (const TetId id : mesh.active_leaves()) {
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
