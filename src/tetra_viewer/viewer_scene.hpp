#pragma once

#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/whole_cell_surface.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace tetra_viewer {

// Standard orbit camera used by both rendering and screen-space LOD. Keeping
// the target independent of the evaluated surface permits real camera
// translation instead of restricting interaction to a sphere-centred orbit.
struct OrbitCamera {
  tetra::Vec3 target{0.5,0.5,0.5};
  double distance{2.5};
  double yaw{};
  double pitch{};

  [[nodiscard]] tetra::Vec3 forward() const {
    const double horizontal=std::cos(pitch);
    return {-horizontal*std::sin(yaw),-std::sin(pitch),
            -horizontal*std::cos(yaw)};
  }

  [[nodiscard]] tetra::Vec3 position() const {
    return target-forward()*distance;
  }

  [[nodiscard]] tetra::Vec3 right() const {
    const auto direction=forward();
    const tetra::Vec3 seed{direction.z,0.0,-direction.x};
    const double length=std::hypot(seed.x,seed.z);
    return length>1.0e-15?seed/length:tetra::Vec3{-1.0,0.0,0.0};
  }

  [[nodiscard]] tetra::Vec3 up() const {
    const auto direction=forward(),horizontal=right();
    return {horizontal.y*direction.z-horizontal.z*direction.y,
            horizontal.z*direction.x-horizontal.x*direction.z,
            horizontal.x*direction.y-horizontal.y*direction.x};
  }

  void orbit(double delta_x,double delta_y,double precision=1.0) {
    constexpr double radians_per_pixel=0.004;
    yaw+=delta_x*radians_per_pixel*precision;
    pitch=std::clamp(pitch+delta_y*radians_per_pixel*precision,-1.45,1.45);
  }

  void pan(double delta_x,double delta_y,double viewport_height,
           double vertical_fov_radians,double precision=1.0) {
    const double pixels=std::max(1.0,viewport_height);
    const double scale=2.0*std::max(distance,0.05)*
        std::tan(vertical_fov_radians*0.5)/pixels*precision;
    target=target-right()*(delta_x*scale)+up()*(delta_y*scale);
  }

  void dolly(double wheel,double precision=1.0) {
    const double step=std::max(distance*0.22,0.05)*precision;
    distance=std::clamp(distance-wheel*step,0.0,20.0);
  }
};

struct ViewportPoint {
  double x{};
  double y{};
  double depth{};
  bool visible{};
};

// Vulkan viewports with a positive height map positive NDC Y down the screen.
// Keeping editor picking on this shared projection prevents rendered gizmos
// and their hit regions from being mirrored vertically.
[[nodiscard]] inline ViewportPoint project_to_vulkan_viewport(
    tetra::Vec3 point,tetra::Vec3 camera_position,tetra::Vec3 forward,
    tetra::Vec3 right,tetra::Vec3 up,double vertical_fov_radians,
    double viewport_width,double viewport_height) {
  const auto offset=point-camera_position;
  const double depth=offset.x*forward.x+offset.y*forward.y+offset.z*forward.z;
  if(depth<=1.0e-4)return {};
  const double horizontal=offset.x*right.x+offset.y*right.y+offset.z*right.z;
  const double vertical=offset.x*up.x+offset.y*up.y+offset.z*up.z;
  const double tangent=std::tan(vertical_fov_radians*0.5);
  const double aspect=viewport_width/viewport_height;
  return {(horizontal/(depth*tangent*aspect)*0.5+0.5)*viewport_width,
          (0.5+vertical/(depth*tangent)*0.5)*viewport_height,depth,true};
}

enum class CameraGizmoMode : std::uint8_t { select,translate,rotate };
enum class CameraGizmoAxis : std::uint8_t { none,x,y,z };

struct LodCameraPose {
  tetra::Vec3 position{0.5,0.5,3.0};
  tetra::Vec3 forward{0.0,0.0,-1.0};
  tetra::Vec3 up{0.0,1.0,0.0};

  [[nodiscard]] static tetra::Vec3 axis(CameraGizmoAxis selected) {
    switch(selected){
      case CameraGizmoAxis::x:return {1.0,0.0,0.0};
      case CameraGizmoAxis::y:return {0.0,1.0,0.0};
      case CameraGizmoAxis::z:return {0.0,0.0,1.0};
      case CameraGizmoAxis::none:return {};
    }
    return {};
  }

  void translate(CameraGizmoAxis selected,double amount) {
    position=position+axis(selected)*amount;
  }

  void rotate(CameraGizmoAxis selected,double radians) {
    const auto rotation_axis=axis(selected);
    if(selected==CameraGizmoAxis::none)return;
    const auto rotate_vector=[&](tetra::Vec3 value){
      const double cosine=std::cos(radians),sine=std::sin(radians);
      const tetra::Vec3 cross{
          rotation_axis.y*value.z-rotation_axis.z*value.y,
          rotation_axis.z*value.x-rotation_axis.x*value.z,
          rotation_axis.x*value.y-rotation_axis.y*value.x};
      const double projection=rotation_axis.x*value.x+
          rotation_axis.y*value.y+rotation_axis.z*value.z;
      return value*cosine+cross*sine+rotation_axis*(projection*(1.0-cosine));
    };
    const auto normalize=[](tetra::Vec3 value){
      const double length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
      return length>1.0e-15?value/length:tetra::Vec3{};
    };
    forward=normalize(rotate_vector(forward));
    up=normalize(rotate_vector(up));
  }

  void apply(tetra::Camera& camera) const {
    camera.position=position;
    camera.forward=forward;
    camera.up=up;
  }
};

enum class ConnectedVertexKind : std::uint8_t {
  hierarchy,
  surface_intersection,
  snapped_surface,
  stencil_interior,
  fixed_outer_surface,
};

enum class ConnectedCellRegion : std::uint8_t {
  hierarchy_core,
  boundary_connector,
  outer_shell,
};

// Complementary element measures used by construction, optimization, and
// scripted regression benchmarks. All normalized shape scores are one for a
// regular tetrahedron and approach zero at degeneracy.
struct TetrahedronQuality {
  double signed_six_volume{};
  double mean_ratio{};
  double volume_surface_longest_edge{};
  double minimum_dihedral_sine{};
  double minimum_dihedral_degrees{};
  double maximum_dihedral_degrees{};
};

[[nodiscard]] TetrahedronQuality evaluate_tetrahedron_quality(
    const std::array<tetra::Vec3,4>& points);

// CPU mirror of the edge fragment's pixel filter, used by deterministic
// raster-coverage regression tests and the scripted renderer.
[[nodiscard]] inline double screen_space_edge_coverage(double distance_pixels) {
  return std::clamp(1.0-std::abs(distance_pixels),0.0,1.0);
}

inline constexpr double screen_space_edge_depth_epsilon=5.0e-6;

[[nodiscard]] inline bool screen_space_edge_depth_passes(double edge_depth,
                                                          double stored_depth) {
  return edge_depth-screen_space_edge_depth_epsilon<=stored_depth;
}

// Analytic triangle-wire coverage shared by the scripted renderer and its
// regression tests. d_barycentric_dx/dy are screen-space derivatives, so the
// result remains stable under edge rotation, triangle scale, and perspective.
[[nodiscard]] inline double wireframe_coverage(
    std::array<double,3> barycentric,
    const std::array<double,3>& d_barycentric_dx,
    const std::array<double,3>& d_barycentric_dy,
    int edge_mask = 7) {
  double closest_edge_pixels=std::numeric_limits<double>::infinity();
  for(std::size_t coordinate=0;coordinate<3;++coordinate){
    if((edge_mask&(1<<coordinate))==0)continue;
    const double pixel_width=std::max(
        std::hypot(d_barycentric_dx[coordinate],d_barycentric_dy[coordinate]),1.0e-12);
    closest_edge_pixels=std::min(closest_edge_pixels,barycentric[coordinate]/pixel_width);
  }
  return screen_space_edge_coverage(closest_edge_pixels);
}

enum class ShadingModel { studio_flat, dihedral_angle, normal_error, reflection_stripes };

inline constexpr std::array shading_models{
    ShadingModel::studio_flat,
    ShadingModel::dihedral_angle,
    ShadingModel::normal_error,
    ShadingModel::reflection_stripes,
};

[[nodiscard]] constexpr std::string_view shading_model_name(ShadingModel model) {
  switch (model) {
    case ShadingModel::studio_flat: return "Studio flat";
    case ShadingModel::dihedral_angle: return "Dihedral angle";
    case ShadingModel::normal_error: return "Normal error";
    case ShadingModel::reflection_stripes: return "Reflection stripes";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view shading_model_key(ShadingModel model) {
  switch (model) {
    case ShadingModel::studio_flat: return "studio-flat";
    case ShadingModel::dihedral_angle: return "dihedral-angle";
    case ShadingModel::normal_error: return "normal-error";
    case ShadingModel::reflection_stripes: return "reflection-stripes";
  }
  return "unknown";
}

enum class SurfaceMethod { full_tetrahedra, marching_tetrahedra, lattice_cleaving,
                           tetrahedral_layer, dual_contouring, surface_optimization };

inline constexpr std::array surface_methods{
    SurfaceMethod::full_tetrahedra,
    SurfaceMethod::marching_tetrahedra,
    SurfaceMethod::lattice_cleaving,
    SurfaceMethod::tetrahedral_layer,
    SurfaceMethod::dual_contouring,
    SurfaceMethod::surface_optimization,
};

[[nodiscard]] constexpr std::string_view surface_method_name(SurfaceMethod method) {
  switch (method) {
    case SurfaceMethod::full_tetrahedra: return "Full-tetrahedron boundary";
    case SurfaceMethod::marching_tetrahedra: return "Marching tetrahedra";
    case SurfaceMethod::lattice_cleaving: return "Lattice-cleaved boundary layer";
    case SurfaceMethod::tetrahedral_layer: return "Extracted tetrahedral layer (experimental)";
    case SurfaceMethod::dual_contouring: return "Dual contour surface (experimental)";
    case SurfaceMethod::surface_optimization: return "Surface optimization (TetWeave-inspired)";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view surface_method_key(SurfaceMethod method) {
  switch (method) {
    case SurfaceMethod::full_tetrahedra: return "full-tetrahedra";
    case SurfaceMethod::marching_tetrahedra: return "marching-tetrahedra";
    case SurfaceMethod::lattice_cleaving: return "lattice-cleaving";
    case SurfaceMethod::tetrahedral_layer: return "tetrahedral-layer";
    case SurfaceMethod::dual_contouring: return "dual-contouring";
    case SurfaceMethod::surface_optimization: return "surface-optimization";
  }
  return "unknown";
}

// Patch locality is expressed in terms of logical hierarchy owners. An
// owner-local extractor can regenerate one owner's conforming children in
// isolation. An incident-edge-star extractor must also see every logical owner
// whose conforming children touch the same primal edge. Global methods cannot
// currently reproduce the monolithic result from a bounded owner halo.
enum class SurfacePatchNeighbourhood : std::uint8_t {
  owner,
  incident_edge_star,
  global,
};

struct SurfacePatchDependency {
  SurfacePatchNeighbourhood neighbourhood{SurfacePatchNeighbourhood::global};
  std::uint8_t halo_steps{std::numeric_limits<std::uint8_t>::max()};
  std::string_view reason{};

  [[nodiscard]] constexpr bool patchable() const noexcept {
    return neighbourhood!=SurfacePatchNeighbourhood::global;
  }
};

[[nodiscard]] constexpr std::string_view surface_patch_neighbourhood_key(
    SurfacePatchNeighbourhood neighbourhood) {
  switch(neighbourhood){
    case SurfacePatchNeighbourhood::owner:return "owner";
    case SurfacePatchNeighbourhood::incident_edge_star:return "incident-edge-star";
    case SurfacePatchNeighbourhood::global:return "global";
  }
  return "unknown";
}

[[nodiscard]] constexpr SurfacePatchDependency surface_patch_dependency(
    SurfaceMethod method) {
  switch(method){
    case SurfaceMethod::marching_tetrahedra:
      return {SurfacePatchNeighbourhood::owner,0U,
              "triangles depend only on one conforming cell"};
    case SurfaceMethod::lattice_cleaving:
      return {SurfacePatchNeighbourhood::owner,0U,
              "displayed boundary uses owner-local marching intersections"};
    case SurfaceMethod::dual_contouring:
      return {SurfacePatchNeighbourhood::incident_edge_star,1U,
              "each dual polygon needs every cell incident to its primal edge"};
    case SurfaceMethod::full_tetrahedra:
      return {SurfacePatchNeighbourhood::global,
              std::numeric_limits<std::uint8_t>::max(),
              "material selection and exposed-face cancellation may be global"};
    case SurfaceMethod::tetrahedral_layer:
      return {SurfacePatchNeighbourhood::global,
              std::numeric_limits<std::uint8_t>::max(),
              "surface welding and shell exterior extraction are global"};
    case SurfaceMethod::surface_optimization:
      return {SurfacePatchNeighbourhood::global,
              std::numeric_limits<std::uint8_t>::max(),
              "welded iterative smoothing and connected shells are globally coupled"};
  }
  return {};
}

// These methods use the same marching-tetrahedra boundary topology as the
// adaptively cleaved volume. They can therefore own a solid cutaway boundary
// without introducing a second, disconnected display surface.
[[nodiscard]] constexpr bool supports_connected_volume(SurfaceMethod method) {
  return method == SurfaceMethod::marching_tetrahedra ||
         method == SurfaceMethod::lattice_cleaving ||
         method == SurfaceMethod::surface_optimization;
}

// Surface extraction and volume conformance are deliberately independent:
// the same displayed surface can be compared over either the original
// full-cell approximation or a locally cleaved, conforming volume.
enum class VolumeConnectionMethod {
  hierarchy_cells,
  fixed_surface_shell,
  coned_prototype,
  quality_stencils,
  adaptive_cleaving,
};

inline constexpr tetra::SubdivisionMethod default_subdivision_method=
    tetra::SubdivisionMethod::bcc_red_green;
inline constexpr tetra::ImplicitShapeKind default_implicit_shape=
    tetra::ImplicitShapeKind::perlin_terrain;
inline constexpr SurfaceMethod default_surface_method=SurfaceMethod::surface_optimization;
inline constexpr VolumeConnectionMethod default_volume_connection_method=
    VolumeConnectionMethod::fixed_surface_shell;

[[nodiscard]] constexpr VolumeConnectionMethod default_volume_connection_for_shape(
    tetra::ImplicitShapeKind kind){
  return kind==tetra::ImplicitShapeKind::perlin_terrain?
      VolumeConnectionMethod::adaptive_cleaving:default_volume_connection_method;
}

inline constexpr std::array volume_connection_methods{
    VolumeConnectionMethod::hierarchy_cells,
    VolumeConnectionMethod::fixed_surface_shell,
    VolumeConnectionMethod::coned_prototype,
    VolumeConnectionMethod::quality_stencils,
    VolumeConnectionMethod::adaptive_cleaving,
};

[[nodiscard]] constexpr bool uses_connected_volume(VolumeConnectionMethod method) {
  return method != VolumeConnectionMethod::hierarchy_cells;
}

[[nodiscard]] constexpr std::string_view volume_connection_method_name(
    VolumeConnectionMethod method) {
  switch (method) {
    case VolumeConnectionMethod::hierarchy_cells: return "Whole hierarchy cells (disconnected comparison)";
    case VolumeConnectionMethod::fixed_surface_shell: return "Connected hierarchy core";
    case VolumeConnectionMethod::coned_prototype: return "Centroid-coned prototype";
    case VolumeConnectionMethod::quality_stencils: return "Quality cleaving stencils";
    case VolumeConnectionMethod::adaptive_cleaving: return "Quality stencils + safe warping";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view volume_connection_method_key(
    VolumeConnectionMethod method) {
  switch (method) {
    case VolumeConnectionMethod::hierarchy_cells: return "hierarchy-cells";
    case VolumeConnectionMethod::fixed_surface_shell: return "fixed-surface-shell";
    case VolumeConnectionMethod::coned_prototype: return "coned-prototype";
    case VolumeConnectionMethod::quality_stencils: return "quality-stencils";
    case VolumeConnectionMethod::adaptive_cleaving: return "adaptive-cleaving";
  }
  return "unknown";
}

// Interactive selections are authoritative. Whole hierarchy cells are a
// useful comparison volume beneath an independently optimized surface, so do
// not silently replace that selection when a cutaway is enabled.
[[nodiscard]] constexpr VolumeConnectionMethod resolve_interactive_volume_connection(
    SurfaceMethod,VolumeConnectionMethod selected,bool) {
  return selected;
}

[[nodiscard]] constexpr bool volume_connection_available(
    SurfaceMethod surface_method,VolumeConnectionMethod candidate) {
  return candidate!=VolumeConnectionMethod::fixed_surface_shell||
         surface_method==SurfaceMethod::surface_optimization;
}

enum class StencilConstruction { fixed, selected };

inline constexpr std::array stencil_constructions{
    StencilConstruction::fixed,
    StencilConstruction::selected,
};

[[nodiscard]] constexpr std::string_view stencil_construction_name(
    StencilConstruction construction) {
  switch(construction){
    case StencilConstruction::fixed: return "Fixed deterministic templates";
    case StencilConstruction::selected: return "Quality-selected template atlas";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view stencil_construction_key(
    StencilConstruction construction) {
  switch(construction){
    case StencilConstruction::fixed: return "fixed";
    case StencilConstruction::selected: return "selected";
  }
  return "unknown";
}

enum class StencilSelectionObjective { surface, balanced, volume };

inline constexpr std::array stencil_selection_objectives{
    StencilSelectionObjective::surface,
    StencilSelectionObjective::balanced,
    StencilSelectionObjective::volume,
};

[[nodiscard]] constexpr std::string_view stencil_selection_objective_name(
    StencilSelectionObjective objective) {
  switch(objective){
    case StencilSelectionObjective::surface: return "Surface fairness";
    case StencilSelectionObjective::balanced: return "Balanced surface and volume";
    case StencilSelectionObjective::volume: return "Tetrahedron quality";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view stencil_selection_objective_key(
    StencilSelectionObjective objective) {
  switch(objective){
    case StencilSelectionObjective::surface: return "surface";
    case StencilSelectionObjective::balanced: return "balanced";
    case StencilSelectionObjective::volume: return "volume";
  }
  return "unknown";
}

enum class MaterialRule {
  all_vertices_inside, centroid_inside, majority_vertices_inside, any_overlap,
  variational_faithful, variational, variational_smooth
};

inline constexpr std::array material_rules{
    MaterialRule::all_vertices_inside,
    MaterialRule::centroid_inside,
    MaterialRule::majority_vertices_inside,
    MaterialRule::any_overlap,
    MaterialRule::variational,
    MaterialRule::variational_faithful,
    MaterialRule::variational_smooth,
};

[[nodiscard]] constexpr std::string_view material_rule_name(MaterialRule rule) {
  switch (rule) {
    case MaterialRule::all_vertices_inside: return "All vertices inside";
    case MaterialRule::centroid_inside: return "Centroid inside";
    case MaterialRule::majority_vertices_inside: return "At least 3 vertices inside";
    case MaterialRule::any_overlap: return "Any sphere overlap";
    case MaterialRule::variational: return "Variational whole-cell cut";
    case MaterialRule::variational_faithful: return "Variational - faithful";
    case MaterialRule::variational_smooth: return "Variational - smooth";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view material_rule_key(MaterialRule rule) {
  switch (rule) {
    case MaterialRule::all_vertices_inside: return "all-vertices";
    case MaterialRule::centroid_inside: return "centroid";
    case MaterialRule::majority_vertices_inside: return "vertex-majority";
    case MaterialRule::any_overlap: return "any-overlap";
    case MaterialRule::variational: return "variational";
    case MaterialRule::variational_faithful: return "variational-faithful";
    case MaterialRule::variational_smooth: return "variational-smooth";
  }
  return "unknown";
}

[[nodiscard]] constexpr bool is_variational_material_rule(MaterialRule rule) {
  return rule==MaterialRule::variational_faithful||rule==MaterialRule::variational||
         rule==MaterialRule::variational_smooth;
}

[[nodiscard]] constexpr tetra::WholeCellOptions whole_cell_options(MaterialRule rule) {
  tetra::WholeCellOptions options;
  if(rule==MaterialRule::variational_faithful){
    options.data_weight=40.0;options.area_weight=0.005;
    options.distance_weight=0.2;options.normal_weight=0.1;
  }else if(rule==MaterialRule::variational_smooth){
    options.data_weight=2.0;options.area_weight=0.12;
    options.distance_weight=0.8;options.normal_weight=0.5;
  }
  return options;
}

struct SceneVertex {
  float position[3];
  float colour[3];
  // A zero normal denotes unlit line geometry.
  float normal[3]{};
  // Maximum adjacent-face dihedral and analytic-normal error, in degrees.
  float diagnostics[2]{};
  // Low three bits select the triangle edges; bit three selects wire-only
  // volume faces. This must not share storage with diagnostic angles.
  float edge_flags{7.0F};
  // Triangle-local coordinates used for anti-aliased surface wireframes.
  float barycentric[3]{};
};

struct PreparedScene {
  std::vector<SceneVertex> triangle_vertices;
  std::vector<SceneVertex> hierarchy_line_vertices;
  // Reserved for non-triangle overlays. Connected exterior-surface edges are
  // rendered from triangle barycentrics so hidden edges cannot show through.
  std::vector<SceneVertex> surface_line_vertices;
  // Replacement volume cells generated only for sign-changing background
  // leaves by the lattice-cleaving experiment.
  std::vector<std::array<tetra::Vec3, 4>> cleaved_cells;
  // Packed derived volume produced by adaptive/unstructured mesh cleaving.
  // Original inside cells and boundary mini-cells share these arrays; every
  // output tetrahedron retains its authoritative hierarchy parent.
  std::vector<tetra::Vec3> connected_volume_vertices;
  std::vector<ConnectedVertexKind> connected_volume_vertex_kinds;
  // Source hierarchy edge for intersection vertices; invalid/invalid for
  // hierarchy and stencil-interior vertices.
  std::vector<std::array<tetra::VertexId,2>> connected_volume_source_edges;
  // Stable topological classification for vertices on the cleaved exterior.
  // Geometry optimization moves these vertices, so their role must not be
  // rediscovered later from a floating-point distance tolerance.
  std::vector<std::uint8_t> connected_volume_surface_vertices;
  std::vector<std::array<std::size_t, 4>> connected_volume_tetrahedra;
  std::vector<tetra::TetId> connected_volume_parents;
  std::vector<std::uint8_t> connected_volume_boundary;
  std::vector<ConnectedCellRegion> connected_volume_regions;
  // Classification is aligned with TetMesh::conforming_volume().
  std::vector<tetra::SurfaceRelation> relations;
  std::vector<std::size_t> depth_counts;
  std::size_t inside_count{};
  std::size_t outside_count{};
  std::size_t intersecting_count{};
  std::size_t selected_count{};
  std::size_t whole_cell_boundary_faces{};
  std::size_t whole_cell_nonmanifold_edges{};
  double whole_cell_selected_volume{};
  double whole_cell_solve_milliseconds{};
  std::uint64_t whole_cell_hash{};
  std::size_t volume_internal_edges{};
  std::size_t volume_boundary_edges{};
  std::size_t visible_volume_face_triangles{};
  std::size_t connected_surface_edges{};
  std::size_t marching_tetrahedra_triangles{};
  std::size_t cleaved_tetrahedra{};
  double cleaved_volume{};
  std::size_t surface_layer_tetrahedra{};
  std::size_t dual_contour_triangles{};
  std::size_t optimized_surface_vertices{};
  std::size_t rejected_surface_moves{};
  std::size_t optimized_volume_boundary_vertices{};
  std::size_t rejected_volume_boundary_moves{};
  std::size_t selected_stencil_cells{};
  std::size_t alternate_stencil_cells{};
  std::uint64_t connected_surface_hash{};
  std::uint64_t standalone_surface_hash{};
  std::size_t hybrid_shell_tetrahedra{};
  std::size_t hybrid_shell_vertices{};
  std::size_t hybrid_recovery_steps{};
  std::size_t hybrid_failed_prisms{};
  std::size_t hybrid_missing_provenance{};
  std::size_t hybrid_inset_failures{};
  std::size_t hybrid_missing_inner_faces{};
  std::size_t hybrid_degenerate_prisms{};
  std::size_t hybrid_unmatched_faces{};
  bool hybrid_volume_valid{};
  double minimum_connected_tet_quality_before{1.0};
  double minimum_connected_tet_quality_after{1.0};
  double minimum_connected_tet_volume_surface_quality_before{1.0};
  double minimum_connected_tet_volume_surface_quality_after{1.0};
  double minimum_connected_tet_dihedral_sine_before{1.0};
  double minimum_connected_tet_dihedral_sine_after{1.0};
  double minimum_connected_tet_dihedral_degrees_after{180.0};
  double maximum_connected_tet_dihedral_degrees_after{};
  double total_volume{};
  double statistics_milliseconds{};
  double upload_preparation_milliseconds{};
  double mean_dihedral_degrees{};
  double percentile95_dihedral_degrees{};
  double percentile99_dihedral_degrees{};
  double maximum_dihedral_degrees{};
  double mean_normal_error_degrees{};
  double percentile95_normal_error_degrees{};
  double percentile99_normal_error_degrees{};
  double maximum_normal_error_degrees{};
  double minimum_surface_triangle_angle_degrees{180.0};
  double maximum_surface_triangle_edge_ratio{1.0};
  bool surface_diagnostics_available{};
  bool summary_statistics_available{};
};

struct SurfaceGeometryHashes {
  std::uint64_t triangle_hash{};
  std::uint64_t edge_hash{};
  std::uint64_t edge_incidence_hash{};
  std::uint64_t material_boundary_hash{};
  std::uint64_t wire_edge_hash{};
  std::size_t triangle_count{};
  std::size_t edge_count{};
  std::size_t edge_incidence_count{};
  std::size_t wire_edge_count{};
  bool operator==(const SurfaceGeometryHashes&) const = default;
};

// Canonical hashes of physical surface geometry. Triangle buffer order and
// cyclic corner rotation do not matter, while winding, coordinates, duplicate
// triangles, missing edges, incidence multiplicity, classification changes,
// incomplete submitted wire edges, and cracks remain observable.
[[nodiscard]] SurfaceGeometryHashes surface_geometry_hashes(
    const PreparedScene& scene);

// Validates the relationship between surface and volume, rather than either
// representation in isolation. The connected exterior must literally be the
// unmatched boundary of the tetrahedral complex.
struct ConnectedComplexValidation {
  bool valid{};
  bool positive_volumes{};
  bool manifold_face_incidence{};
  bool exterior_owned_by_surface{};
  bool regions_aligned{};
  bool graded_parent_band{};
  std::size_t exterior_faces{};
  std::size_t nonmanifold_faces{};
  std::size_t unmatched_non_surface_faces{};
  double minimum_signed_six_volume{};
  double maximum_adjacent_edge_ratio{1.0};
  double maximum_adjacent_parent_edge_ratio{1.0};
  unsigned int maximum_adjacent_parent_depth_difference{};
  ConnectedCellRegion maximum_ratio_first_region{ConnectedCellRegion::hierarchy_core};
  ConnectedCellRegion maximum_ratio_second_region{ConnectedCellRegion::hierarchy_core};
};

[[nodiscard]] ConnectedComplexValidation validate_connected_complex(
    const PreparedScene& scene,const tetra::TetMesh* hierarchy=nullptr);

struct ProjectionStatistics {
  std::size_t pending_count{};
  std::size_t accepted_count{};
};

struct SurfacePatchRecord {
  tetra::TetId logical_owner{tetra::invalid_tet};
  std::uint64_t mesh_revision{};
  std::uint64_t field_revision{};
  std::uint64_t topology_hash{};
  std::size_t triangle_begin{};
  std::size_t triangle_count{};
  std::size_t triangle_capacity{};
  tetra::Vec3 bounds_minimum{};
  tetra::Vec3 bounds_maximum{};
};

struct SurfacePatchMetrics {
  bool active{};
  bool monolithic_fallback{};
  bool global_fallback{};
  bool full_rebuild{};
  std::size_t dirty_owners{};
  std::size_t rebuilt_patches{};
  std::size_t reused_patches{};
  std::size_t retired_patches{};
  std::size_t generated_triangles{};
  std::size_t reused_triangles{};
  std::size_t output_triangles{};
  std::size_t arena_slots{};
  std::size_t free_slots{};
  std::size_t retained_bytes{};
  double update_milliseconds{};
};

struct SurfaceDrawChunkRecord {
  std::size_t arena_slot{};
  std::size_t triangle_count{};
  std::size_t segment_begin{};
  std::size_t segment_count{};
  std::uint64_t content_revision{};
};

struct SurfaceDrawPatchSegment {
  tetra::TetId logical_owner{tetra::invalid_tet};
  std::size_t source_triangle_offset{};
  std::size_t chunk_index{};
  std::size_t triangle_begin{};
  std::size_t triangle_count{};
};

struct SurfaceDrawChunkFreeRange {
  std::size_t begin_slot{};
  std::size_t slot_count{};
};

struct SurfaceDrawChunkMetrics {
  std::size_t chunk_capacity{};
  std::size_t source_patches{};
  std::size_t nonempty_patches{};
  std::size_t patch_segments{};
  std::size_t triangles{};
  std::size_t active_chunks{};
  std::size_t retained_slots{};
  std::size_t free_slots{};
  std::size_t reused_slots{};
  std::size_t allocated_slots{};
  std::size_t released_slots{};
  std::size_t dirty_patches{};
  std::size_t dirty_chunks{};
  std::size_t reused_chunks{};
  std::size_t reused_bytes{};
  std::size_t local_repacks{};
  std::size_t overflow_splits{};
  std::size_t underfull_merges{};
  std::size_t chunk_splits{};
  std::size_t chunk_merges{};
  std::size_t global_compactions{};
  std::size_t fragmented_slots{};
  std::size_t fragmentation_bytes{};
  std::size_t copied_bytes{};
  std::size_t retained_bytes{};
  std::size_t draw_calls{};
  double occupancy{};
  double pack_milliseconds{};
};

// Fixed-capacity storage is independent of the hierarchy and owner-patch
// arenas. A patch may span several chunks and several patches may share one
// chunk; all metadata and geometry remain in flat retained arrays.
class SurfaceDrawChunkStorage {
 public:
  explicit SurfaceDrawChunkStorage(std::size_t chunk_capacity=256U);

  void pack(std::span<const SurfacePatchRecord> patches,
            std::span<const tetra::Triangle> patch_arena);

  [[nodiscard]] std::size_t chunk_capacity() const noexcept {
    return chunk_capacity_;
  }
  [[nodiscard]] std::span<const SurfaceDrawChunkRecord> chunks() const noexcept {
    return chunks_;
  }
  [[nodiscard]] std::span<const SurfaceDrawPatchSegment> segments() const noexcept {
    return segments_;
  }
  [[nodiscard]] std::span<const SurfaceDrawChunkFreeRange> free_ranges() const noexcept {
    return free_ranges_;
  }
  [[nodiscard]] std::span<const tetra::Triangle> arena() const noexcept {
    return arena_;
  }
  [[nodiscard]] const SurfaceDrawChunkMetrics& metrics() const noexcept {
    return metrics_;
  }

 private:
  struct RetainedPatch {
    tetra::TetId logical_owner{tetra::invalid_tet};
    std::uint64_t field_revision{};
    std::uint64_t topology_hash{};
    std::size_t triangle_count{};
  };

  [[nodiscard]] std::size_t allocate_slot();
  void release_active_slots();
  void normalize_free_ranges();
  void compact(std::span<const SurfacePatchRecord> patches,
               std::span<const tetra::Triangle> patch_arena);
  void finish_metrics(std::size_t triangle_count,
                      std::chrono::steady_clock::time_point start);

  std::size_t chunk_capacity_{};
  std::vector<SurfaceDrawChunkRecord> chunks_;
  std::vector<SurfaceDrawPatchSegment> segments_;
  std::vector<SurfaceDrawChunkFreeRange> free_ranges_;
  std::vector<tetra::Triangle> arena_;
  std::vector<RetainedPatch> retained_patches_;
  SurfaceDrawChunkMetrics metrics_;
  std::uint64_t next_content_revision_{1U};
};

// Deliberately simple full-packing oracle used to prove retained chunk output
// independently of incremental packing and later retained upload ranges.
[[nodiscard]] std::vector<tetra::Triangle> direct_pack_surface_patches(
    std::span<const SurfacePatchRecord> patches,
    std::span<const tetra::Triangle> patch_arena);
[[nodiscard]] std::vector<tetra::Triangle> assemble_surface_draw_chunks(
    const SurfaceDrawChunkStorage& storage);

struct SurfaceHostDrawRange {
  std::size_t host_slot{};
  std::size_t source_arena_slot{};
  std::uint64_t source_content_revision{};
  std::uint64_t host_content_revision{};
  std::size_t triangle_vertex_begin{};
  std::size_t triangle_vertex_count{};
  // The native wire pass reads the same vertices as the solid pass. Keeping
  // the alias explicit prevents a second staging copy from creeping back in.
  std::size_t wire_vertex_begin{};
  std::size_t wire_vertex_count{};
};

struct SurfaceHostFreeRange {
  std::size_t begin_slot{};
  std::size_t slot_count{};
};

struct SurfaceHostStagingMetrics {
  std::uint64_t publication_generation{};
  std::size_t source_chunks{};
  std::size_t active_ranges{};
  std::size_t retained_slots{};
  std::size_t free_slots{};
  std::size_t dirty_ranges{};
  std::size_t reused_ranges{};
  std::size_t allocated_slots{};
  std::size_t reused_slots{};
  std::size_t released_slots{};
  std::size_t staged_triangle_bytes{};
  std::size_t staged_wire_bytes{};
  std::size_t aliased_wire_bytes{};
  std::size_t retained_bytes{};
  double stage_milliseconds{};
};

// Transactional retained CPU staging for the surface draw front. New or
// changed source chunks are copied into unused host slots, then the complete
// ordered table is published in one swap. The preceding ranges and bytes are
// never overwritten while the replacement is being assembled.
class SurfaceHostStagingStorage {
 public:
  explicit SurfaceHostStagingStorage(std::size_t triangle_chunk_capacity=256U);

  void stage(const SurfaceDrawChunkStorage& source,
             std::span<const SceneVertex> logical_vertices);

  [[nodiscard]] std::size_t triangle_chunk_capacity() const noexcept {
    return triangle_chunk_capacity_;
  }
  [[nodiscard]] std::size_t vertex_slot_capacity() const noexcept {
    return triangle_chunk_capacity_*3U;
  }
  [[nodiscard]] std::span<const SurfaceHostDrawRange> ranges() const noexcept {
    return ranges_;
  }
  [[nodiscard]] std::span<const SurfaceHostFreeRange> free_ranges() const noexcept {
    return free_ranges_;
  }
  [[nodiscard]] std::span<const SceneVertex> arena() const noexcept {
    return arena_;
  }
  [[nodiscard]] const SurfaceHostStagingMetrics& metrics() const noexcept {
    return metrics_;
  }

 private:
  [[nodiscard]] std::size_t allocate_slot(
      std::vector<SurfaceHostFreeRange>& candidate_free_ranges,
      SurfaceHostStagingMetrics& candidate_metrics);
  static void normalize_free_ranges(
      std::vector<SurfaceHostFreeRange>& ranges);

  std::size_t triangle_chunk_capacity_{};
  std::vector<SurfaceHostDrawRange> ranges_;
  std::vector<SurfaceHostDrawRange> range_scratch_;
  std::vector<SurfaceHostFreeRange> free_ranges_;
  std::vector<SceneVertex> arena_;
  SurfaceHostStagingMetrics metrics_;
  std::uint64_t next_content_revision_{1U};
};

[[nodiscard]] std::vector<SceneVertex> assemble_surface_host_staging(
    const SurfaceHostStagingStorage& storage);

struct SurfaceDeviceUploadRange {
  std::size_t source_vertex_begin{};
  std::size_t destination_vertex_begin{};
  std::size_t vertex_count{};
};

struct SurfaceDeviceDrawRange {
  std::size_t first_vertex{};
  std::size_t vertex_count{};
  bool operator==(const SurfaceDeviceDrawRange&) const = default;
};

struct SurfaceDeviceUploadMetrics {
  std::uint64_t source_generation{};
  bool prepared{};
  bool full_reallocation{};
  std::size_t required_vertex_capacity{};
  std::size_t upload_ranges{};
  std::size_t reused_ranges{};
  std::size_t uploaded_bytes{};
  std::size_t draw_calls{};
};

// Shared by the Vulkan renderer and headless device-memory oracle. prepare()
// builds a complete candidate without changing the published draw front;
// commit() atomically promotes it after every requested range was copied.
class SurfaceDeviceUploadPlanner {
 public:
  void prepare(const SurfaceHostStagingStorage& host,
               std::size_t device_vertex_capacity);
  void commit();
  void cancel() noexcept;
  void reset() noexcept;

  [[nodiscard]] std::span<const SurfaceDeviceUploadRange> uploads() const noexcept {
    return upload_scratch_;
  }
  [[nodiscard]] std::span<const SurfaceDeviceDrawRange> candidate_draws() const noexcept {
    return draw_scratch_;
  }
  [[nodiscard]] std::span<const SurfaceDeviceDrawRange> published_draws() const noexcept {
    return published_draws_;
  }
  [[nodiscard]] const SurfaceDeviceUploadMetrics& metrics() const noexcept {
    return metrics_;
  }
  [[nodiscard]] std::uint64_t published_generation() const noexcept {
    return published_generation_;
  }

 private:
  struct SlotKey {
    std::uint64_t content_revision{};
    std::size_t vertex_count{};
    bool operator==(const SlotKey&) const = default;
  };

  std::vector<SlotKey> published_slots_;
  std::vector<SlotKey> slot_scratch_;
  std::vector<SurfaceDeviceUploadRange> upload_scratch_;
  std::vector<SurfaceDeviceDrawRange> published_draws_;
  std::vector<SurfaceDeviceDrawRange> draw_scratch_;
  SurfaceDeviceUploadMetrics metrics_;
  std::uint64_t published_generation_{};
};

void apply_surface_device_upload_plan(
    SurfaceDeviceUploadPlanner& planner,
    const SurfaceHostStagingStorage& host,
    std::vector<SceneVertex>& device_arena);
[[nodiscard]] std::vector<SceneVertex> assemble_surface_device_publication(
    const SurfaceDeviceUploadPlanner& planner,
    std::span<const SceneVertex> device_arena);

// Geometry preparation is also used by headless research scripts, which need
// all measurements.  The interactive viewer can explicitly omit measurements
// that are neither displayed nor used by the selected shading model.
struct ScenePreparationOptions {
  bool surface_diagnostics{true};
  bool summary_statistics{true};
};

// Expands each two-vertex line segment into the six vertices consumed by the
// screen-space ribbon pipeline. The headless publication benchmark and Vulkan
// renderer share this exact CPU upload-staging operation.
void expand_line_segments_for_upload(
    std::span<const SceneVertex> line_vertices,
    std::vector<SceneVertex>& ribbons);

// Builds camera-independent CPU scene data. Packed 64-bit edge keys are
// sorted once per revision; there are no per-edge node allocations.
[[nodiscard]] PreparedScene prepare_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere,
                                          SurfaceMethod surface_method, MaterialRule material_rule,
                                          bool show_faces, bool show_hierarchy_edges,
                                          bool show_surface_edges,
                                          bool depth_colours, bool show_volume_edges = false,
                                          bool show_volume_faces = false,
                                          double x_cut_position = 0.5,
                                          VolumeConnectionMethod volume_connection_method =
                                              VolumeConnectionMethod::quality_stencils,
                                          StencilConstruction stencil_construction =
                                              StencilConstruction::fixed,
                                          StencilSelectionObjective stencil_selection_objective =
                                              StencilSelectionObjective::balanced,
                                          ScenePreparationOptions preparation = {},
                                          std::span<const tetra::Triangle> surface_override = {},
                                          bool surface_override_is_owner_patches = false);
[[nodiscard]] inline PreparedScene prepare_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere,
                                                 SurfaceMethod surface_method, MaterialRule material_rule,
                                                 bool show_faces, bool show_edges, bool depth_colours) {
  return prepare_scene(mesh, sphere, surface_method, material_rule,
                       show_faces, show_edges, show_edges, depth_colours, false);
}
[[nodiscard]] inline PreparedScene prepare_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere,
                                                 MaterialRule material_rule, bool show_faces, bool show_edges,
                                                 bool depth_colours) {
  return prepare_scene(mesh, sphere, SurfaceMethod::full_tetrahedra, material_rule,
                       show_faces, show_edges, show_edges, depth_colours, false);
}

// Projection state changes while orbiting, without invalidating geometry or
// classification. This pass only visits intersecting leaves.
[[nodiscard]] ProjectionStatistics prepare_projection_statistics(
    const tetra::TetMesh& mesh, const PreparedScene& scene, const tetra::Camera& camera, double pixel_threshold);

class SceneCache {
 public:
  // Returns true only when CPU geometry/classification was rebuilt.
  bool update_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere, std::uint64_t sphere_revision,
                    SurfaceMethod surface_method, MaterialRule material_rule,
                    bool show_faces, bool show_hierarchy_edges, bool show_surface_edges,
                    bool depth_colours, bool show_volume_edges = false,
                    bool show_volume_faces = false,
                    double x_cut_position = 0.5,
                    VolumeConnectionMethod volume_connection_method =
                        VolumeConnectionMethod::adaptive_cleaving,
                    StencilConstruction stencil_construction =
                        StencilConstruction::fixed,
                    StencilSelectionObjective stencil_selection_objective =
                        StencilSelectionObjective::balanced,
                    ScenePreparationOptions preparation = {},
                    std::span<const tetra::Triangle> surface_override = {},
                    std::uint64_t surface_override_revision = 0);
  bool update_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere, std::uint64_t sphere_revision,
                    SurfaceMethod surface_method, MaterialRule material_rule,
                    bool show_faces, bool show_edges, bool depth_colours) {
    return update_scene(mesh, sphere, sphere_revision, surface_method, material_rule,
                        show_faces, show_edges, show_edges, depth_colours);
  }
  bool update_scene(const tetra::TetMesh& mesh, const tetra::Sphere& sphere, std::uint64_t sphere_revision,
                    MaterialRule material_rule, bool show_faces, bool show_edges, bool depth_colours) {
    return update_scene(mesh, sphere, sphere_revision, SurfaceMethod::full_tetrahedra, material_rule,
                        show_faces, show_edges, show_edges, depth_colours);
  }
  // Returns true only when camera-dependent statistics were recomputed.
  bool update_projection(const tetra::TetMesh& mesh, const tetra::Camera& camera, double pixel_threshold);

  [[nodiscard]] const PreparedScene& scene() const noexcept { return scene_; }
  [[nodiscard]] const ProjectionStatistics& projection() const noexcept { return projection_; }
  [[nodiscard]] std::uint64_t scene_generation() const noexcept { return scene_generation_; }
  [[nodiscard]] std::uint64_t projection_generation() const noexcept { return projection_generation_; }
  [[nodiscard]] bool initialized() const noexcept { return has_subdivision_method_; }
  [[nodiscard]] std::uint64_t mesh_revision() const noexcept { return mesh_revision_; }
  [[nodiscard]] std::span<const SurfacePatchRecord> surface_patch_records() const noexcept {
    return surface_patch_records_;
  }
  [[nodiscard]] std::span<const tetra::Triangle> surface_patch_arena() const noexcept {
    return surface_patch_arena_;
  }
  [[nodiscard]] const SurfacePatchMetrics& surface_patch_metrics() const noexcept {
    return surface_patch_metrics_;
  }

 private:
  struct SurfacePatchFreeRange {
    std::size_t begin{};
    std::size_t count{};
  };
  struct SurfacePatchOwnerCell {
    tetra::TetId owner{tetra::invalid_tet};
    tetra::TetId cell{tetra::invalid_tet};
  };

  void update_surface_patches(
      const tetra::TetMesh& mesh,const tetra::Sphere& sphere,
      std::uint64_t field_revision,SurfaceMethod surface_method);
  void set_surface_patch_fallback(bool monolithic_fallback,bool global_fallback);

  PreparedScene scene_;
  PreparedScene base_scene_;
  ProjectionStatistics projection_;
  std::uint64_t mesh_revision_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t sphere_revision_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t surface_override_revision_{std::numeric_limits<std::uint64_t>::max()};
  tetra::SubdivisionMethod subdivision_method_{tetra::SubdivisionMethod::maubach_diamond};
  bool has_subdivision_method_{};
  std::uint64_t projected_scene_generation_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t scene_generation_{};
  std::uint64_t projection_generation_{};
  tetra::Camera projected_camera_{};
  double projected_pixel_threshold_{std::numeric_limits<double>::quiet_NaN()};
  bool show_faces_{};
  bool show_hierarchy_edges_{};
  bool show_surface_edges_{};
  bool show_volume_edges_{};
  bool show_volume_faces_{};
  double x_cut_position_{std::numeric_limits<double>::quiet_NaN()};
  std::vector<tetra::TetId> volume_material_tetrahedra_;
  std::vector<tetra::TetId> volume_boundary_tetrahedra_;
  std::vector<std::array<tetra::VertexId,3>> volume_boundary_faces_;
  bool volume_classification_valid_{};
  bool depth_colours_{};
  MaterialRule material_rule_{MaterialRule::all_vertices_inside};
  SurfaceMethod surface_method_{SurfaceMethod::full_tetrahedra};
  VolumeConnectionMethod volume_connection_method_{VolumeConnectionMethod::adaptive_cleaving};
  StencilConstruction stencil_construction_{StencilConstruction::fixed};
  StencilSelectionObjective stencil_selection_objective_{StencilSelectionObjective::balanced};
  bool surface_diagnostics_available_{};
  bool summary_statistics_available_{};
  std::vector<SurfacePatchRecord> surface_patch_records_;
  std::vector<SurfacePatchRecord> surface_patch_record_scratch_;
  std::vector<tetra::Triangle> surface_patch_arena_;
  std::vector<tetra::Triangle> surface_patch_output_;
  std::vector<tetra::Triangle> surface_patch_triangle_scratch_;
  std::vector<SurfacePatchFreeRange> surface_patch_free_ranges_;
  std::vector<tetra::TetId> surface_patch_owner_scratch_;
  std::vector<std::uint64_t> surface_patch_topology_hash_scratch_;
  std::vector<tetra::TetId> surface_patch_dirty_scratch_;
  std::vector<tetra::TetId> surface_patch_incident_dirty_scratch_;
  std::vector<tetra::TetId> surface_patch_cell_scratch_;
  std::vector<SurfacePatchOwnerCell> surface_patch_owner_cells_;
  tetra::DualContourPatchBuilder dual_patch_builder_;
  std::vector<tetra::DualContourPatchDependency> dual_patch_dependencies_;
  std::vector<tetra::DualContourPatchTriangle> dual_patch_triangle_scratch_;
  SurfacePatchMetrics surface_patch_metrics_;
  std::uint64_t surface_patch_mesh_revision_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t surface_patch_field_revision_{std::numeric_limits<std::uint64_t>::max()};
  tetra::SubdivisionMethod surface_patch_subdivision_method_{
      tetra::SubdivisionMethod::maubach_diamond};
  bool surface_patch_dual_topology_{};
  bool surface_patch_initialized_{};
};

}  // namespace tetra_viewer
