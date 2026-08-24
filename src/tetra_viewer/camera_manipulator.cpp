#include "tetra_viewer/camera_manipulator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tetra_viewer {
namespace {

[[nodiscard]] double dot(tetra::Vec3 a,tetra::Vec3 b) {
  return a.x*b.x+a.y*b.y+a.z*b.z;
}

[[nodiscard]] tetra::Vec3 cross(tetra::Vec3 a,tetra::Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}

[[nodiscard]] double length(tetra::Vec3 value) { return std::sqrt(dot(value,value)); }

[[nodiscard]] tetra::Vec3 normalized(tetra::Vec3 value,tetra::Vec3 fallback={}) {
  const double magnitude=length(value);
  return magnitude>1.0e-15?value/magnitude:fallback;
}

[[nodiscard]] CameraHandle axis_handle(CameraGizmoMode mode,std::size_t axis) {
  if(mode==CameraGizmoMode::translate)
    return std::array{CameraHandle::move_x,CameraHandle::move_y,
                      CameraHandle::move_z}[axis];
  return std::array{CameraHandle::rotate_x,CameraHandle::rotate_y,
                    CameraHandle::rotate_z}[axis];
}

[[nodiscard]] ViewportPoint project(const ManipulatorView& view,tetra::Vec3 point) {
  return project_to_vulkan_viewport(
      point,view.position,view.forward,view.right,view.up,
      view.vertical_fov_radians,view.viewport_width,view.viewport_height);
}

[[nodiscard]] double segment_distance(double x,double y,ViewportPoint first,
                                      ViewportPoint second) {
  if(!first.visible||!second.visible)return std::numeric_limits<double>::infinity();
  const double dx=second.x-first.x,dy=second.y-first.y;
  const double squared=dx*dx+dy*dy;
  const double amount=squared>1.0e-12?std::clamp(
      ((x-first.x)*dx+(y-first.y)*dy)/squared,0.0,1.0):0.0;
  return std::hypot(x-(first.x+amount*dx),y-(first.y+amount*dy));
}

[[nodiscard]] bool point_in_triangle(double x,double y,
                                     const std::array<ViewportPoint,3>& triangle) {
  if(std::ranges::any_of(triangle,[](const auto& point){return !point.visible;}))return false;
  const double area=(triangle[1].x-triangle[0].x)*(triangle[2].y-triangle[0].y)-
      (triangle[1].y-triangle[0].y)*(triangle[2].x-triangle[0].x);
  if(std::abs(area)<=1.0e-8)return false;
  const auto edge=[](const ViewportPoint& a,const ViewportPoint& b,double px,double py){
    return (px-a.x)*(b.y-a.y)-(py-a.y)*(b.x-a.x);
  };
  const double a=edge(triangle[0],triangle[1],x,y);
  const double b=edge(triangle[1],triangle[2],x,y);
  const double c=edge(triangle[2],triangle[0],x,y);
  return (a>=0.0&&b>=0.0&&c>=0.0)||(a<=0.0&&b<=0.0&&c<=0.0);
}

[[nodiscard]] unsigned int handle_priority(CameraHandle handle) {
  switch(handle){
    case CameraHandle::move_view:
    case CameraHandle::rotate_arcball:return 0U;
    case CameraHandle::move_xy:
    case CameraHandle::move_xz:
    case CameraHandle::move_yz:return 1U;
    case CameraHandle::move_x:
    case CameraHandle::move_y:
    case CameraHandle::move_z:
    case CameraHandle::rotate_x:
    case CameraHandle::rotate_y:
    case CameraHandle::rotate_z:return 2U;
    case CameraHandle::rotate_view:return 3U;
    case CameraHandle::none:return 4U;
  }
  return 4U;
}

[[nodiscard]] std::optional<std::size_t> handle_axis_index(CameraHandle handle) {
  switch(handle){
    case CameraHandle::move_x:case CameraHandle::rotate_x:return 0U;
    case CameraHandle::move_y:case CameraHandle::rotate_y:return 1U;
    case CameraHandle::move_z:case CameraHandle::rotate_z:return 2U;
    default:return std::nullopt;
  }
}

[[nodiscard]] bool same_pose(const LodCameraPose& first,const LodCameraPose& second) {
  return first.position.x==second.position.x&&first.position.y==second.position.y&&
      first.position.z==second.position.z&&first.forward.x==second.forward.x&&
      first.forward.y==second.forward.y&&first.forward.z==second.forward.z&&
      first.up.x==second.up.x&&first.up.y==second.up.y&&first.up.z==second.up.z;
}

[[nodiscard]] double snapped(double value,double step,bool enabled) {
  return enabled&&step>0.0?std::round(value/step)*step:value;
}

[[nodiscard]] tetra::Vec3 plane_normal_for_axis(tetra::Vec3 axis,
                                                const ManipulatorView& view) {
  auto normal=view.forward-axis*dot(view.forward,axis);
  if(length(normal)<=1.0e-8)normal=view.up-axis*dot(view.up,axis);
  if(length(normal)<=1.0e-8)normal=view.right-axis*dot(view.right,axis);
  return normalized(normal,{0.0,1.0,0.0});
}

} // namespace

void orthonormalize_camera_pose(LodCameraPose& pose) {
  pose.forward=normalized(pose.forward,{0.0,0.0,-1.0});
  tetra::Vec3 right=cross(pose.forward,pose.up);
  if(length(right)<=1.0e-12){
    const tetra::Vec3 fallback=std::abs(pose.forward.y)<0.95?
        tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{1.0,0.0,0.0};
    right=cross(pose.forward,fallback);
  }
  right=normalized(right,{1.0,0.0,0.0});
  pose.up=normalized(cross(right,pose.forward),{0.0,1.0,0.0});
}

void rotate_camera_pose(LodCameraPose& pose,tetra::Vec3 world_axis,double radians) {
  world_axis=normalized(world_axis,{0.0,1.0,0.0});
  const auto rotate=[&](tetra::Vec3 value){
    const double cosine=std::cos(radians),sine=std::sin(radians);
    return value*cosine+cross(world_axis,value)*sine+
        world_axis*(dot(world_axis,value)*(1.0-cosine));
  };
  pose.forward=rotate(pose.forward);
  pose.up=rotate(pose.up);
  orthonormalize_camera_pose(pose);
}

ManipulatorBasis manipulator_basis(const LodCameraPose& input,ManipulatorSpace space) {
  if(space==ManipulatorSpace::world)return {};
  auto pose=input;
  orthonormalize_camera_pose(pose);
  const auto right=normalized(cross(pose.forward,pose.up),{1.0,0.0,0.0});
  // Local Z follows the camera's backward axis, matching a conventional
  // right-handed transform frame while forward remains visually explicit.
  return {right,pose.up,pose.forward*-1.0};
}

double manipulator_world_scale(const ManipulatorView& view,tetra::Vec3 pivot,
                               double desired_radius_pixels) {
  const double depth=dot(pivot-view.position,view.forward);
  if(!(depth>1.0e-6)||!(view.viewport_height>0.0)||
     !(view.vertical_fov_radians>0.0))return 0.0;
  const double units_per_pixel=2.0*depth*std::tan(view.vertical_fov_radians*0.5)/
      view.viewport_height;
  return std::clamp(desired_radius_pixels*units_per_pixel,1.0e-6,1.0e6);
}

ManipulatorRay manipulator_view_ray(const ManipulatorView& view,double cursor_x,
                                    double cursor_y) {
  const double width=std::max(view.viewport_width,1.0);
  const double height=std::max(view.viewport_height,1.0);
  const double ndc_x=2.0*cursor_x/width-1.0;
  const double ndc_y=2.0*cursor_y/height-1.0;
  const double tangent=std::tan(view.vertical_fov_radians*0.5);
  const double aspect=width/height;
  const auto direction=normalized(view.forward+view.right*(ndc_x*tangent*aspect)+
                                  view.up*(ndc_y*tangent),view.forward);
  return {view.position,direction};
}

CameraHandleGeometry build_camera_handle_geometry(
    const LodCameraPose& pose,CameraGizmoMode mode,ManipulatorSpace space,
    const ManipulatorView& view,double desired_radius_pixels) {
  CameraHandleGeometry geometry;
  geometry.pivot=pose.position;
  geometry.mode=mode;
  geometry.basis=manipulator_basis(pose,space);
  geometry.world_scale=manipulator_world_scale(view,pose.position,desired_radius_pixels);
  if(geometry.world_scale<=0.0||mode==CameraGizmoMode::select)return geometry;
  const std::array axes{geometry.basis.x,geometry.basis.y,geometry.basis.z};
  if(mode==CameraGizmoMode::translate){
    for(std::size_t index=0;index<axes.size();++index){
      const auto handle=axis_handle(mode,index);
      const auto end=pose.position+axes[index]*geometry.world_scale;
      geometry.segments.push_back({handle,pose.position,end});
      const auto side_a=axes[(index+1U)%3U];
      const auto side_b=axes[(index+2U)%3U];
      const auto base=pose.position+axes[index]*(geometry.world_scale*0.84);
      const double radius=geometry.world_scale*0.055;
      constexpr std::size_t cone_sides=12U;
      for(std::size_t side=0;side<cone_sides;++side){
        const double first_angle=2.0*std::acos(-1.0)*
            static_cast<double>(side)/static_cast<double>(cone_sides);
        const double second_angle=2.0*std::acos(-1.0)*
            static_cast<double>(side+1U)/static_cast<double>(cone_sides);
        const auto rim=[&](double angle){return base+
            side_a*(radius*std::cos(angle))+side_b*(radius*std::sin(angle));};
        geometry.triangles.push_back({handle,{{
            end,rim(first_angle),rim(second_angle)}}});
        geometry.triangles.push_back({handle,{{
            base,rim(second_angle),rim(first_angle)}}});
      }
    }
    constexpr std::array plane_handles{
        CameraHandle::move_xy,CameraHandle::move_xz,CameraHandle::move_yz};
    constexpr std::array<std::array<std::size_t,2>,3> plane_axes{{{{0,1}},{{0,2}},{{1,2}}}};
    for(std::size_t index=0;index<plane_handles.size();++index){
      const auto first=axes[plane_axes[index][0]],second=axes[plane_axes[index][1]];
      const auto inner=geometry.world_scale*0.18,outer=geometry.world_scale*0.34;
      const auto a=pose.position+first*inner+second*inner;
      const auto b=pose.position+first*outer+second*inner;
      const auto c=pose.position+first*outer+second*outer;
      const auto d=pose.position+first*inner+second*outer;
      geometry.quads.push_back({plane_handles[index],{{a,b,c,d}},true});
    }
    const double centre=geometry.world_scale*0.075;
    geometry.quads.push_back({CameraHandle::move_view,{{
        pose.position-view.right*centre-view.up*centre,
        pose.position+view.right*centre-view.up*centre,
        pose.position+view.right*centre+view.up*centre,
        pose.position-view.right*centre+view.up*centre}},false});
  }else{
    for(std::size_t index=0;index<axes.size();++index){
      const auto first=axes[(index+1U)%3U];
      auto second=normalized(cross(axes[index],first));
      geometry.rings.push_back({axis_handle(mode,index),pose.position,axes[index],
                                first,second,geometry.world_scale*0.82});
    }
    geometry.rings.push_back({CameraHandle::rotate_view,pose.position,view.forward,
                              view.right,view.up,geometry.world_scale});
    // Maya exposes free rotation through the interior of the axis sphere. A
    // small centre loop communicates that affordance without adding a second
    // large ring that competes with the constrained axes.
    geometry.rings.push_back({CameraHandle::rotate_arcball,pose.position,view.forward,
                              view.right,view.up,geometry.world_scale*0.09});
  }
  return geometry;
}

CameraFrustumGeometry build_lod_camera_frustum(
    const LodCameraPose& input,const tetra::Camera& camera,
    const ManipulatorView& view,double desired_depth_pixels) {
  CameraFrustumGeometry geometry;
  auto pose=input;
  orthonormalize_camera_pose(pose);
  const double depth=manipulator_world_scale(view,pose.position,desired_depth_pixels);
  if(depth<=0.0)return geometry;
  const auto right=normalized(cross(pose.forward,pose.up),{1.0,0.0,0.0});
  const auto up=normalized(cross(right,pose.forward),{0.0,1.0,0.0});
  const double half_height=depth*std::tan(camera.vertical_fov_radians*0.5);
  const double half_width=half_height*std::max(camera.aspect_ratio,1.0e-6);
  const auto centre=pose.position+pose.forward*depth;
  const std::array corners{
      centre+right*half_width+up*half_height,
      centre-right*half_width+up*half_height,
      centre-right*half_width-up*half_height,
      centre+right*half_width-up*half_height};
  for(const auto corner:corners)
    geometry.segments.push_back({CameraHandle::none,pose.position,corner});
  for(std::size_t index=0;index<corners.size();++index)
    geometry.segments.push_back({CameraHandle::none,corners[index],
                                 corners[(index+1U)%corners.size()]});
  // Up marker on the near rectangle makes camera roll immediately visible.
  geometry.segments.push_back({CameraHandle::none,
      centre+up*half_height,
      centre+up*(half_height*1.35)});
  return geometry;
}

ManipulatorHit hit_test_camera_handles(const CameraHandleGeometry& geometry,
                                       const ManipulatorView& view,double cursor_x,
                                       double cursor_y,double tolerance_pixels) {
  ManipulatorHit best{CameraHandle::none,tolerance_pixels,
                      std::numeric_limits<double>::infinity()};
  const auto consider=[&](CameraHandle handle,double distance,double depth){
    if(distance>tolerance_pixels)return;
    const auto priority=handle_priority(handle),best_priority=handle_priority(best.handle);
    if(distance+1.0e-6<best.distance_pixels||
       (std::abs(distance-best.distance_pixels)<=1.0e-6&&priority<best_priority)||
       (std::abs(distance-best.distance_pixels)<=1.0e-6&&priority==best_priority&&
        depth<best.depth))best={handle,distance,depth};
  };
  const auto pivot=project(view,geometry.pivot);
  if(pivot.visible&&geometry.mode==CameraGizmoMode::translate)
    consider(CameraHandle::move_view,
             std::max(0.0,std::hypot(cursor_x-pivot.x,cursor_y-pivot.y)-7.0),
             pivot.depth);
  for(const auto& triangle:geometry.triangles){
    std::array<ViewportPoint,3> points{};
    for(std::size_t index=0;index<3;++index)points[index]=project(view,triangle.points[index]);
    if(point_in_triangle(cursor_x,cursor_y,points)){
      const double depth=(points[0].depth+points[1].depth+points[2].depth)/3.0;
      consider(triangle.handle,0.0,depth);
    }
  }
  for(const auto& quad:geometry.quads){
    std::array<ViewportPoint,4> points{};
    for(std::size_t index=0;index<4U;++index)points[index]=project(view,quad.points[index]);
    const std::array first{points[0],points[1],points[2]};
    const std::array second{points[0],points[2],points[3]};
    if(point_in_triangle(cursor_x,cursor_y,first)||
       point_in_triangle(cursor_x,cursor_y,second)){
      const double depth=(points[0].depth+points[1].depth+points[2].depth+
                          points[3].depth)*0.25;
      consider(quad.handle,0.0,depth);
    }
  }
  for(const auto& segment:geometry.segments){
    const auto first=project(view,segment.first),second=project(view,segment.second);
    consider(segment.handle,segment_distance(cursor_x,cursor_y,first,second),
             std::min(first.depth,second.depth));
  }
  constexpr std::size_t ring_segments=96;
  for(const auto& ring:geometry.rings){
    for(std::size_t index=0;index<ring_segments;++index){
      const double a=2.0*std::acos(-1.0)*static_cast<double>(index)/ring_segments;
      const double b=2.0*std::acos(-1.0)*static_cast<double>(index+1U)/ring_segments;
      const auto point=[&](double angle){return ring.centre+
          (ring.first_basis*std::cos(angle)+ring.second_basis*std::sin(angle))*ring.radius;};
      const auto first=project(view,point(a)),second=project(view,point(b));
      consider(ring.handle,segment_distance(cursor_x,cursor_y,first,second),
               std::min(first.depth,second.depth));
    }
  }
  if(best.handle==CameraHandle::none&&geometry.mode==CameraGizmoMode::rotate&&
     pivot.visible){
    const auto radius_point=project(
        view,geometry.pivot+view.right*(geometry.world_scale*0.60));
    if(radius_point.visible&&
       std::hypot(cursor_x-pivot.x,cursor_y-pivot.y)<=
           std::hypot(radius_point.x-pivot.x,radius_point.y-pivot.y))
      best={CameraHandle::rotate_arcball,0.0,pivot.depth};
  }
  return best;
}

std::optional<double> closest_axis_parameter(
    ManipulatorRay ray,tetra::Vec3 axis_origin,tetra::Vec3 axis_direction,
    double parallel_epsilon) {
  ray.direction=normalized(ray.direction);
  axis_direction=normalized(axis_direction);
  const auto offset=ray.origin-axis_origin;
  const double rd=dot(ray.direction,axis_direction);
  const double denominator=1.0-rd*rd;
  if(denominator<=parallel_epsilon)return std::nullopt;
  return (dot(offset,axis_direction)-dot(offset,ray.direction)*rd)/denominator;
}

std::optional<tetra::Vec3> intersect_drag_plane(
    ManipulatorRay ray,tetra::Vec3 plane_point,tetra::Vec3 plane_normal,
    double parallel_epsilon) {
  ray.direction=normalized(ray.direction);
  plane_normal=normalized(plane_normal);
  const double denominator=dot(ray.direction,plane_normal);
  if(std::abs(denominator)<=parallel_epsilon)return std::nullopt;
  const double amount=dot(plane_point-ray.origin,plane_normal)/denominator;
  return ray.origin+ray.direction*amount;
}

double signed_rotation_angle(tetra::Vec3 start,tetra::Vec3 current,tetra::Vec3 axis) {
  axis=normalized(axis,{0.0,0.0,1.0});
  start=normalized(start-axis*dot(start,axis));
  current=normalized(current-axis*dot(current,axis));
  return std::atan2(dot(axis,cross(start,current)),dot(start,current));
}

tetra::Vec3 arcball_vector(double cursor_x,double cursor_y,double centre_x,
                           double centre_y,double radius_pixels) {
  const double radius=std::max(radius_pixels,1.0);
  double x=(cursor_x-centre_x)/radius;
  double y=(centre_y-cursor_y)/radius;
  const double squared=x*x+y*y;
  if(squared<=0.5)return {x,y,std::sqrt(1.0-squared)};
  const double radial=std::sqrt(squared);
  const double z=0.5/radial;
  const double normalization=1.0/std::sqrt(squared+z*z);
  return {x*normalization,y*normalization,z*normalization};
}

bool CameraManipulator::begin_drag(CameraHandle handle,const LodCameraPose& pose,
                                   const ManipulatorView& view,double cursor_x,
                                   double cursor_y) {
  if(handle==CameraHandle::none||mode==CameraGizmoMode::select)return false;
  drag_start_pose_=pose;
  drag_start_view_=view;
  drag_basis_=manipulator_basis(pose,space);
  active=handle;
  dragging_=true;
  displayed_delta_=0.0;
  drag_start_cursor_x_=cursor_x;
  drag_start_cursor_y_=cursor_y;
  axis_uses_plane_fallback_=false;
  const auto ray=manipulator_view_ray(view,cursor_x,cursor_y);
  const std::array axes{drag_basis_.x,drag_basis_.y,drag_basis_.z};
  if(const auto index=handle_axis_index(handle);index.has_value()){
    drag_axis_=axes[*index];
    if(handle==CameraHandle::move_x||handle==CameraHandle::move_y||
       handle==CameraHandle::move_z){
      if(const auto parameter=closest_axis_parameter(ray,pose.position,drag_axis_))
        drag_start_parameter_=*parameter;
      else{
        axis_uses_plane_fallback_=true;
        drag_plane_normal_=plane_normal_for_axis(drag_axis_,view);
        const auto point=intersect_drag_plane(ray,pose.position,drag_plane_normal_);
        if(!point){dragging_=false;active=CameraHandle::none;return false;}
        drag_start_parameter_=dot(*point-pose.position,drag_axis_);
      }
      return true;
    }
  }
  if(handle==CameraHandle::move_xy||handle==CameraHandle::move_xz||
     handle==CameraHandle::move_yz||handle==CameraHandle::move_view){
    if(handle==CameraHandle::move_xy)drag_plane_normal_=axes[2];
    else if(handle==CameraHandle::move_xz)drag_plane_normal_=axes[1];
    else if(handle==CameraHandle::move_yz)drag_plane_normal_=axes[0];
    else drag_plane_normal_=view.forward;
    const auto point=intersect_drag_plane(ray,pose.position,drag_plane_normal_);
    if(!point){dragging_=false;active=CameraHandle::none;return false;}
    drag_start_point_=*point;
    return true;
  }
  if(handle==CameraHandle::rotate_view)drag_axis_=view.forward;
  if(handle==CameraHandle::rotate_arcball){
    const auto pivot=project(view,pose.position);
    if(!pivot.visible){dragging_=false;active=CameraHandle::none;return false;}
    drag_start_vector_=arcball_vector(cursor_x,cursor_y,pivot.x,pivot.y,82.0);
    return true;
  }
  if(handle==CameraHandle::rotate_x||handle==CameraHandle::rotate_y||
     handle==CameraHandle::rotate_z||handle==CameraHandle::rotate_view){
    const auto point=intersect_drag_plane(ray,pose.position,drag_axis_);
    if(point)drag_start_vector_=normalized(*point-pose.position);
    else drag_start_vector_={};
    return true;
  }
  dragging_=false;active=CameraHandle::none;
  return false;
}

bool CameraManipulator::update_drag(LodCameraPose& pose,const ManipulatorView& view,
                                    double cursor_x,double cursor_y,double precision) {
  if(!dragging_)return false;
  const auto ray=manipulator_view_ray(view,cursor_x,cursor_y);
  pose=drag_start_pose_;
  const std::array axes{drag_basis_.x,drag_basis_.y,drag_basis_.z};
  double delta{};
  if(active==CameraHandle::move_x||active==CameraHandle::move_y||
     active==CameraHandle::move_z){
    std::optional<double> parameter;
    if(axis_uses_plane_fallback_){
      if(const auto point=intersect_drag_plane(ray,drag_start_pose_.position,
                                               drag_plane_normal_))
        parameter=dot(*point-drag_start_pose_.position,drag_axis_);
    }else parameter=closest_axis_parameter(ray,drag_start_pose_.position,drag_axis_);
    if(!parameter)return false;
    delta=(*parameter-drag_start_parameter_)*precision;
    if(snap.enabled){
      if(snap.mode==ManipulatorSnapSettings::Mode::relative)
        delta=snapped(delta,snap.translation_step,true);
      else{
        const double start_coordinate=dot(drag_start_pose_.position,drag_axis_);
        delta=snapped(start_coordinate+delta,snap.translation_step,true)-start_coordinate;
      }
    }
    pose.position=drag_start_pose_.position+drag_axis_*delta;
  }else if(active==CameraHandle::move_xy||active==CameraHandle::move_xz||
           active==CameraHandle::move_yz||active==CameraHandle::move_view){
    const auto point=intersect_drag_plane(ray,drag_start_pose_.position,drag_plane_normal_);
    if(!point)return false;
    auto movement=(*point-drag_start_point_)*precision;
    if(active!=CameraHandle::move_view){
      std::array<std::size_t,2> allowed{};
      if(active==CameraHandle::move_xy)allowed={0U,1U};
      else if(active==CameraHandle::move_xz)allowed={0U,2U};
      else allowed={1U,2U};
      double first=dot(movement,axes[allowed[0]]);
      double second=dot(movement,axes[allowed[1]]);
      if(snap.enabled&&snap.mode==ManipulatorSnapSettings::Mode::absolute){
        const double first_start=dot(drag_start_pose_.position,axes[allowed[0]]);
        const double second_start=dot(drag_start_pose_.position,axes[allowed[1]]);
        first=snapped(first_start+first,snap.translation_step,true)-first_start;
        second=snapped(second_start+second,snap.translation_step,true)-second_start;
      }else{
        first=snapped(first,snap.translation_step,snap.enabled);
        second=snapped(second,snap.translation_step,snap.enabled);
      }
      movement=axes[allowed[0]]*first+axes[allowed[1]]*second;
    }else if(snap.enabled){
      double horizontal=dot(movement,view.right),vertical=dot(movement,view.up);
      if(snap.mode==ManipulatorSnapSettings::Mode::absolute){
        const double horizontal_start=dot(drag_start_pose_.position,view.right);
        const double vertical_start=dot(drag_start_pose_.position,view.up);
        horizontal=snapped(horizontal_start+horizontal,snap.translation_step,true)-
            horizontal_start;
        vertical=snapped(vertical_start+vertical,snap.translation_step,true)-vertical_start;
      }else{
        horizontal=snapped(horizontal,snap.translation_step,true);
        vertical=snapped(vertical,snap.translation_step,true);
      }
      movement=view.right*horizontal+view.up*vertical;
    }
    pose.position=drag_start_pose_.position+movement;
    delta=length(movement);
  }else if(active==CameraHandle::rotate_arcball){
    const auto pivot=project(view,drag_start_pose_.position);
    if(!pivot.visible)return false;
    const auto current=arcball_vector(cursor_x,cursor_y,pivot.x,pivot.y,82.0);
    const auto local_axis=cross(drag_start_vector_,current);
    const double sine=length(local_axis);
    delta=std::atan2(sine,std::clamp(dot(drag_start_vector_,current),-1.0,1.0));
    delta=snapped(delta*precision,snap.rotation_step_radians,snap.enabled);
    if(sine>1.0e-12){
      const auto normalized_axis=local_axis/sine;
      const auto world_axis=view.right*normalized_axis.x+view.up*normalized_axis.y-
          view.forward*normalized_axis.z;
      rotate_camera_pose(pose,world_axis,delta);
    }
  }else{
    const auto point=intersect_drag_plane(ray,drag_start_pose_.position,drag_axis_);
    if(point&&length(drag_start_vector_)>1.0e-12){
      delta=signed_rotation_angle(drag_start_vector_,*point-drag_start_pose_.position,
                                  drag_axis_);
    }else{
      const auto pivot=project(view,drag_start_pose_.position);
      const double first=std::atan2(drag_start_cursor_y_-pivot.y,
                                    drag_start_cursor_x_-pivot.x);
      const double current=std::atan2(cursor_y-pivot.y,cursor_x-pivot.x);
      delta=current-first;
      if(delta>std::acos(-1.0))delta-=2.0*std::acos(-1.0);
      if(delta<-std::acos(-1.0))delta+=2.0*std::acos(-1.0);
    }
    delta=snapped(delta*precision,snap.rotation_step_radians,snap.enabled);
    rotate_camera_pose(pose,drag_axis_,delta);
  }
  displayed_delta_=delta;
  return !same_pose(pose,drag_start_pose_);
}

bool CameraManipulator::finish_drag(const LodCameraPose& pose) {
  if(!dragging_)return false;
  dragging_=false;
  active=CameraHandle::none;
  if(same_pose(pose,drag_start_pose_)){displayed_delta_=0.0;return false;}
  history_.resize(history_cursor_);
  history_.push_back({drag_start_pose_,pose});
  history_cursor_=history_.size();
  return true;
}

bool CameraManipulator::cancel_drag(LodCameraPose& pose) {
  if(!dragging_)return false;
  pose=drag_start_pose_;
  dragging_=false;
  active=CameraHandle::none;
  displayed_delta_=0.0;
  return true;
}

bool CameraManipulator::undo(LodCameraPose& pose) {
  if(!can_undo()||dragging_)return false;
  --history_cursor_;
  pose=history_[history_cursor_].before;
  return true;
}

bool CameraManipulator::redo(LodCameraPose& pose) {
  if(!can_redo()||dragging_)return false;
  pose=history_[history_cursor_].after;
  ++history_cursor_;
  return true;
}

void EmptyViewportGesture::begin(bool camera_selected,double cursor_x,
                                 double cursor_y,
                                 bool explicit_navigation_modifier) noexcept {
  press_x_=cursor_x;
  press_y_=cursor_y;
  pending_deselect_=camera_selected&&!explicit_navigation_modifier;
}

void EmptyViewportGesture::update(double cursor_x,double cursor_y) noexcept {
  constexpr double navigation_threshold_pixels=3.0;
  if(pending_deselect_&&
     std::hypot(cursor_x-press_x_,cursor_y-press_y_)>=navigation_threshold_pixels)
    pending_deselect_=false;
}

bool EmptyViewportGesture::finish_should_deselect() noexcept {
  const bool result=pending_deselect_;
  pending_deselect_=false;
  return result;
}

} // namespace tetra_viewer
