#pragma once

#include "tetra_viewer/viewer_scene.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace tetra_viewer {

enum class ManipulatorSpace : std::uint8_t { world,local };

enum class CameraHandle : std::uint8_t {
  none,
  move_x,move_y,move_z,
  move_xy,move_xz,move_yz,
  move_view,
  rotate_x,rotate_y,rotate_z,
  rotate_view,rotate_arcball,
};

[[nodiscard]] constexpr std::string_view camera_handle_name(CameraHandle handle) {
  switch(handle){
    case CameraHandle::none:return "None";
    case CameraHandle::move_x:return "Move X";
    case CameraHandle::move_y:return "Move Y";
    case CameraHandle::move_z:return "Move Z";
    case CameraHandle::move_xy:return "Move XY";
    case CameraHandle::move_xz:return "Move XZ";
    case CameraHandle::move_yz:return "Move YZ";
    case CameraHandle::move_view:return "Move in view";
    case CameraHandle::rotate_x:return "Rotate X";
    case CameraHandle::rotate_y:return "Rotate Y";
    case CameraHandle::rotate_z:return "Rotate Z";
    case CameraHandle::rotate_view:return "Rotate view";
    case CameraHandle::rotate_arcball:return "Arcball";
  }
  return "None";
}

[[nodiscard]] constexpr CameraHandle preferred_axis_handle(
    CameraGizmoMode mode,std::size_t axis) {
  if(axis>2U)return CameraHandle::none;
  if(mode==CameraGizmoMode::translate)
    return std::array{CameraHandle::move_x,CameraHandle::move_y,
                      CameraHandle::move_z}[axis];
  if(mode==CameraGizmoMode::rotate)
    return std::array{CameraHandle::rotate_x,CameraHandle::rotate_y,
                      CameraHandle::rotate_z}[axis];
  return CameraHandle::none;
}

[[nodiscard]] constexpr bool manipulator_pointer_input_allowed(
    bool deterministic_visual_check,bool controls_hovered,
    bool imgui_wants_mouse) {
  return !deterministic_visual_check&&!controls_hovered&&!imgui_wants_mouse;
}

struct ManipulatorBasis {
  tetra::Vec3 x{1.0,0.0,0.0};
  tetra::Vec3 y{0.0,1.0,0.0};
  tetra::Vec3 z{0.0,0.0,1.0};
};

struct ManipulatorView {
  tetra::Vec3 position{};
  tetra::Vec3 forward{0.0,0.0,-1.0};
  tetra::Vec3 right{1.0,0.0,0.0};
  tetra::Vec3 up{0.0,1.0,0.0};
  double vertical_fov_radians{0.7853981633974483};
  double viewport_width{1.0};
  double viewport_height{1.0};
};

struct ManipulatorRay {
  tetra::Vec3 origin{};
  tetra::Vec3 direction{0.0,0.0,-1.0};
};

struct ManipulatorSegment {
  CameraHandle handle{CameraHandle::none};
  tetra::Vec3 first{};
  tetra::Vec3 second{};
};

struct ManipulatorTriangle {
  CameraHandle handle{CameraHandle::none};
  std::array<tetra::Vec3,3> points{};
};

struct ManipulatorQuad {
  CameraHandle handle{CameraHandle::none};
  std::array<tetra::Vec3,4> points{};
  bool filled{};
};

struct ManipulatorRing {
  CameraHandle handle{CameraHandle::none};
  tetra::Vec3 centre{};
  tetra::Vec3 normal{0.0,0.0,1.0};
  tetra::Vec3 first_basis{1.0,0.0,0.0};
  tetra::Vec3 second_basis{0.0,1.0,0.0};
  double radius{};
};

struct CameraHandleGeometry {
  tetra::Vec3 pivot{};
  CameraGizmoMode mode{CameraGizmoMode::select};
  ManipulatorBasis basis{};
  double world_scale{};
  std::vector<ManipulatorSegment> segments;
  std::vector<ManipulatorTriangle> triangles;
  std::vector<ManipulatorQuad> quads;
  std::vector<ManipulatorRing> rings;
};

struct CameraFrustumGeometry {
  std::vector<ManipulatorSegment> segments;
};

struct ManipulatorHit {
  CameraHandle handle{CameraHandle::none};
  double distance_pixels{};
  double depth{};
};

struct ManipulatorSnapSettings {
  enum class Mode : std::uint8_t { relative,absolute };
  bool enabled{};
  Mode mode{Mode::relative};
  double translation_step{0.05};
  double rotation_step_radians{0.2617993877991494};
};

class CameraManipulator {
 public:
  CameraGizmoMode mode{CameraGizmoMode::select};
  ManipulatorSpace space{ManipulatorSpace::world};
  CameraHandle hovered{CameraHandle::none};
  CameraHandle preferred{CameraHandle::none};
  CameraHandle active{CameraHandle::none};
  ManipulatorSnapSettings snap{};

  [[nodiscard]] bool dragging() const noexcept{return dragging_;}
  [[nodiscard]] bool begin_drag(CameraHandle handle,const LodCameraPose& pose,
                                const ManipulatorView& view,double cursor_x,
                                double cursor_y);
  [[nodiscard]] bool update_drag(LodCameraPose& pose,const ManipulatorView& view,
                                 double cursor_x,double cursor_y,
                                 double precision=1.0);
  [[nodiscard]] bool finish_drag(const LodCameraPose& pose);
  [[nodiscard]] bool cancel_drag(LodCameraPose& pose);
  [[nodiscard]] bool undo(LodCameraPose& pose);
  [[nodiscard]] bool redo(LodCameraPose& pose);
  [[nodiscard]] bool can_undo() const noexcept{return history_cursor_>0U;}
  [[nodiscard]] bool can_redo() const noexcept{return history_cursor_<history_.size();}
  [[nodiscard]] double displayed_delta() const noexcept{return displayed_delta_;}

 private:
  struct PoseEdit { LodCameraPose before,after; };
  LodCameraPose drag_start_pose_{};
  ManipulatorView drag_start_view_{};
  ManipulatorBasis drag_basis_{};
  tetra::Vec3 drag_axis_{};
  tetra::Vec3 drag_plane_normal_{};
  tetra::Vec3 drag_start_point_{};
  tetra::Vec3 drag_start_vector_{};
  double drag_start_parameter_{};
  double drag_start_cursor_x_{};
  double drag_start_cursor_y_{};
  double displayed_delta_{};
  bool dragging_{};
  bool axis_uses_plane_fallback_{};
  std::vector<PoseEdit> history_;
  std::size_t history_cursor_{};
};

class EmptyViewportGesture {
 public:
  void begin(bool camera_selected,double cursor_x,double cursor_y,
             bool explicit_navigation_modifier=false) noexcept;
  void update(double cursor_x,double cursor_y) noexcept;
  [[nodiscard]] bool finish_should_deselect() noexcept;
  void cancel() noexcept{pending_deselect_=false;}
  [[nodiscard]] bool pending_deselect() const noexcept{return pending_deselect_;}

 private:
  double press_x_{};
  double press_y_{};
  bool pending_deselect_{};
};

[[nodiscard]] ManipulatorBasis manipulator_basis(
    const LodCameraPose& pose,ManipulatorSpace space);
[[nodiscard]] double manipulator_world_scale(
    const ManipulatorView& view,tetra::Vec3 pivot,double desired_radius_pixels);
[[nodiscard]] ManipulatorRay manipulator_view_ray(
    const ManipulatorView& view,double cursor_x,double cursor_y);
[[nodiscard]] CameraHandleGeometry build_camera_handle_geometry(
    const LodCameraPose& pose,CameraGizmoMode mode,ManipulatorSpace space,
    const ManipulatorView& view,double desired_radius_pixels=96.0);
[[nodiscard]] CameraFrustumGeometry build_lod_camera_frustum(
    const LodCameraPose& pose,const tetra::Camera& camera,
    const ManipulatorView& view,double desired_depth_pixels=40.0);
[[nodiscard]] ManipulatorHit hit_test_camera_handles(
    const CameraHandleGeometry& geometry,const ManipulatorView& view,
    double cursor_x,double cursor_y,double tolerance_pixels=10.0);

[[nodiscard]] std::optional<double> closest_axis_parameter(
    ManipulatorRay ray,tetra::Vec3 axis_origin,tetra::Vec3 axis_direction,
    double parallel_epsilon=1.0e-8);
[[nodiscard]] std::optional<tetra::Vec3> intersect_drag_plane(
    ManipulatorRay ray,tetra::Vec3 plane_point,tetra::Vec3 plane_normal,
    double parallel_epsilon=1.0e-8);
[[nodiscard]] double signed_rotation_angle(
    tetra::Vec3 start,tetra::Vec3 current,tetra::Vec3 axis);
[[nodiscard]] tetra::Vec3 arcball_vector(
    double cursor_x,double cursor_y,double centre_x,double centre_y,double radius_pixels);

void orthonormalize_camera_pose(LodCameraPose& pose);
void rotate_camera_pose(LodCameraPose& pose,tetra::Vec3 world_axis,double radians);

} // namespace tetra_viewer
