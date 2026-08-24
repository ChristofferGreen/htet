#pragma once

#include "tetra_core/adaptation.hpp"

#include <limits>
#include <span>

namespace tetra {

enum class ImplicitShapeKind : std::uint8_t {
  sphere,
  merging_spheres,
  cube,
  capped_cylinder,
  perlin_terrain,
  torus,
  cone,
  gyroid,
  rounded_cube,
};

inline constexpr std::array<ImplicitShapeKind,9> implicit_shape_kinds{
    ImplicitShapeKind::sphere,ImplicitShapeKind::merging_spheres,
    ImplicitShapeKind::cube,ImplicitShapeKind::capped_cylinder,
    ImplicitShapeKind::perlin_terrain,ImplicitShapeKind::torus,
    ImplicitShapeKind::cone,ImplicitShapeKind::gyroid,
    ImplicitShapeKind::rounded_cube};

[[nodiscard]] std::string_view implicit_shape_name(ImplicitShapeKind kind);
[[nodiscard]] std::string_view implicit_shape_key(ImplicitShapeKind kind);
[[nodiscard]] double implicit_shape_default_secondary(ImplicitShapeKind kind);

struct Sphere {
  Vec3 centre{0.5, 0.5, 0.5};
  double radius{0.35};
  ImplicitShapeKind kind{ImplicitShapeKind::sphere};
  double secondary{0.12};
  double frequency{3.0};

  [[nodiscard]] double signed_distance(Vec3 point) const;
  [[nodiscard]] Vec3 normal(Vec3 point) const;
  [[nodiscard]] Vec3 edge_intersection(Vec3 first,Vec3 second) const;
  [[nodiscard]] Vec3 project_to_surface(Vec3 point) const;
};

struct Camera {
  Vec3 position{0.5, 0.5, 3.0};
  double vertical_fov_radians{0.7853981633974483};
  double viewport_height_pixels{800.0};
  Vec3 forward{0.0,0.0,-1.0};
  Vec3 up{0.0,1.0,0.0};
  double aspect_ratio{1.0};
};

// Camera-only projection state is prepared once per LOD request.  Keeping the
// public batch interface architecture-neutral lets the implementation select
// an AArch64 NEON kernel without leaking vector types into mesh storage.
struct PreparedCameraProjection {
  Vec3 position{};
  Vec3 forward{};
  Vec3 right{};
  Vec3 up{};
  double tangent{};
  double horizontal_tangent{};
  double focal_length{};
  double viewport_height_pixels{};
};

[[nodiscard]] PreparedCameraProjection prepare_camera_projection(const Camera& camera);
[[nodiscard]] double projected_tetrahedron_diameter(const TetMesh& mesh, TetId tet, const Camera& camera);
[[nodiscard]] double projected_tetrahedron_diameter(
    const TetMesh& mesh,TetId tet,const PreparedCameraProjection& camera);
void projected_tetrahedron_diameters(
    const TetMesh& mesh,std::span<const TetId> tetrahedra,
    const PreparedCameraProjection& camera,std::span<double> output);

// Evaluates independent points in batches.  Scalar semantics remain the
// reference; supported builds use isolated SIMD kernels internally.
void evaluate_signed_distances(
    const Sphere& surface,std::span<const Vec3> points,std::span<double> output);
[[nodiscard]] std::vector<TetId> mark_oversized_intersections(const TetMesh& mesh, const Sphere& sphere, const Camera& camera, double pixel_threshold);

struct AdaptiveResult { std::size_t iterations{}; std::size_t refined_leaves{}; bool reached_depth_limit{}; };

// Camera motion changes projected size, but not the implicit field sampled at
// persistent mesh vertices.  A viewer can retain this cache across camera
// reconciliations and clear it when replacing the mesh.
struct ImplicitValueCache {
  std::vector<double> vertex_distances;
  Sphere sampled_surface{};
  bool has_sampled_surface{};
  void clear() noexcept {
    vertex_distances.clear();
    has_sampled_surface=false;
  }
};

struct AdaptationSummaryLayer {
  std::vector<TetId> addresses;
  std::vector<Vec3> spatial_minimum;
  std::vector<Vec3> spatial_maximum;
  std::vector<double> field_minimum;
  std::vector<double> field_maximum;
  std::vector<unsigned int> deepest_resident_depth;
  std::vector<unsigned int> deepest_active_depth;
  std::vector<std::uint64_t> pinned_descendant_words;
};

// Retained transaction frontier aligned by hierarchy layer. Status and mark
// use one bit per address; commands use two bits (keep=0, split=1, merge=2).
struct PackedAdaptationLayer {
  std::vector<TetId> addresses;
  std::vector<std::uint64_t> current_status_words;
  std::vector<std::uint64_t> desired_mark_words;
  std::vector<std::uint64_t> command_words;
};

struct SpatialOwnerRun {
  std::uint32_t begin{};
  std::uint32_t count{};
  Vec3 minimum{};
  Vec3 maximum{};
  double field_minimum{};
  double field_maximum{};
};

struct PersistentSchedulerEntry {
  TetId address{invalid_tet};
  std::uint64_t state_revision{};
  double last_priority{};
};

// Field intervals are stored in packed arrays aligned with the resident mesh
// layers. They are rebuilt only when resident red storage grows or the field
// revision changes; camera motion reuses them.
struct AdaptationPlanningCache {
  std::vector<AdaptationSummaryLayer> layers;
  std::vector<PackedAdaptationLayer> transaction_layers;
  std::uint64_t field_revision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t resident_revision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t pinned_revision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t active_revision{std::numeric_limits<std::uint64_t>::max()};
  std::size_t resident_red_records{};
  std::size_t resident_vertices{};
  std::vector<SpatialOwnerRun> spatial_runs;
  std::uint64_t spatial_index_active_revision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t spatial_index_field_revision{std::numeric_limits<std::uint64_t>::max()};
  std::vector<PersistentSchedulerEntry> split_queue;
  std::vector<PersistentSchedulerEntry> merge_queue;
  // A converged stationary request is an exact no-op until either the mesh,
  // field, camera, or adaptation settings change.
  bool has_stationary_no_change{};
  std::uint64_t stationary_mesh_revision{};
  std::uint64_t stationary_field_revision{};
  Sphere stationary_surface{};
  Camera stationary_camera{};
  AdaptationConfiguration stationary_configuration{};
  double stationary_pixel_threshold{};
  unsigned int stationary_maximum_depth{};
  // A camera pose change opens a complete merge phase after any required
  // splits. Stationary protection is restored only after both phases converge.
  bool has_split_pose{};
  std::uint64_t split_pose_field_revision{};
  Sphere split_pose_surface{};
  Camera split_pose_camera{};
  AdaptationConfiguration split_pose_configuration{};
  double split_pose_pixel_threshold{};
  unsigned int split_pose_maximum_depth{};
  bool has_last_request_origin{};
  Vec3 last_request_origin{};
  Vec3 last_request_forward{};
  Vec3 last_request_up{};
  bool pose_merge_pending{};
  void clear() noexcept {
    layers.clear();
    transaction_layers.clear();
    field_revision=std::numeric_limits<std::uint64_t>::max();
    resident_revision=std::numeric_limits<std::uint64_t>::max();
    pinned_revision=std::numeric_limits<std::uint64_t>::max();
    active_revision=std::numeric_limits<std::uint64_t>::max();
    resident_red_records=0;
    resident_vertices=0;
    spatial_runs.clear();
    spatial_index_active_revision=std::numeric_limits<std::uint64_t>::max();
    spatial_index_field_revision=std::numeric_limits<std::uint64_t>::max();
    split_queue.clear();
    merge_queue.clear();
    has_stationary_no_change=false;
    has_split_pose=false;
    has_last_request_origin=false;
    pose_merge_pending=false;
  }
};

struct SurfaceHierarchyLayer {
  std::vector<TetId> relevant_addresses;
  std::vector<TetId> minimal_addresses;
  std::vector<std::uint64_t> active_words;
  std::vector<std::uint64_t> topology_creation_words;
};

// A field-specific hierarchy is independent of the camera cut.  Relevant
// storage retains every surface cluster and its ancestry; minimal storage
// retains only terminal active clusters and branching/topology-creation
// clusters.  Both are flat packed arrays grouped by hierarchy layer.
struct FixedFieldSurfaceHierarchy {
  std::vector<SurfaceHierarchyLayer> layers;
  std::uint64_t field_revision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t source_resident_revision{std::numeric_limits<std::uint64_t>::max()};
  std::size_t rebuild_count{};
  std::size_t relevant_clusters{};
  std::size_t minimal_clusters{};
  std::size_t retained_bytes{};
  void clear() noexcept {
    layers.clear();
    field_revision=std::numeric_limits<std::uint64_t>::max();
    source_resident_revision=std::numeric_limits<std::uint64_t>::max();
    relevant_clusters=0;
    minimal_clusters=0;
    retained_bytes=0;
  }
};

// Returns true only when field or resident hierarchy changes require a new
// fixed-field representation. Camera motion alone is always a no-op here.
[[nodiscard]] bool update_fixed_field_surface_hierarchy(
    FixedFieldSurfaceHierarchy& hierarchy,const TetMesh& mesh,
    const Sphere& sphere,std::uint64_t field_revision);

[[nodiscard]] std::vector<TetId> select_fixed_field_surface_cut(
    const FixedFieldSurfaceHierarchy& hierarchy,const TetMesh& mesh,
    const Camera& camera,double pixel_threshold,unsigned int maximum_depth,
    LodUpdateStrategy strategy);

[[nodiscard]] std::vector<TetId> query_relevant_surface_hierarchy(
    const FixedFieldSurfaceHierarchy& hierarchy,const TetMesh& mesh,
    Vec3 minimum,Vec3 maximum);

struct PreorderSurfaceHierarchy {
  std::vector<TetId> addresses;
  std::vector<std::uint32_t> descendant_counts;
  // Eight packed direct-child slots per node; UINT32_MAX means absent.
  std::vector<std::uint32_t> child_indices;
  std::vector<std::uint32_t> roots;
  std::uint64_t field_revision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t source_resident_revision{std::numeric_limits<std::uint64_t>::max()};
  std::size_t rebuild_count{};
  std::size_t retained_bytes{};
};

struct Triangle;

struct PreorderRenderMetrics {
  std::size_t nodes_visited{};
  std::size_t selected_nodes{};
  std::size_t generated_triangles{};
  double traversal_ms{};
};

[[nodiscard]] bool update_preorder_surface_hierarchy(
    PreorderSurfaceHierarchy& preorder,
    const FixedFieldSurfaceHierarchy& fixed_field);
[[nodiscard]] PreorderRenderMetrics render_preorder_surface(
    const PreorderSurfaceHierarchy& preorder,const TetMesh& mesh,
    const Sphere& sphere,const Camera& camera,double pixel_threshold,
    unsigned int maximum_depth,std::vector<Triangle>& retained_triangles);

[[nodiscard]] AdaptiveResult refine_to_sphere(
    TetMesh& mesh,const Sphere& sphere,const Camera& camera,double pixel_threshold,
    unsigned int maximum_depth,ImplicitValueCache* value_cache=nullptr);

// Build a deterministic, non-mutating one-generation split/merge transaction
// against the logical red-owner cut. Commands are budgeted and stamped with
// the hierarchy and field revisions that were classified.
[[nodiscard]] AdaptationPlan plan_adaptation(
    const TetMesh& mesh,const Sphere& sphere,const Camera& camera,
    double pixel_threshold,unsigned int maximum_depth,
    const AdaptationConfiguration& configuration={},
    std::uint64_t field_revision=0,
    AdaptationPlanningCache* planning_cache=nullptr);

// Commit only a plan made against the current hierarchy revision. Complete
// BCC sibling families are merged before the disjoint split frontier is
// refined; rejected and stale plans leave the mesh unchanged.
[[nodiscard]] AdaptationCommitResult commit_adaptation(
    TetMesh& mesh,const AdaptationPlan& plan,
    const AdaptationConfiguration& current_configuration,
    std::uint64_t current_field_revision=0);

[[nodiscard]] AdaptationCommitResult adapt_to_surface(
    TetMesh& mesh,const Sphere& sphere,const Camera& camera,
    double pixel_threshold,unsigned int maximum_depth,
    const AdaptationConfiguration& configuration={},
    std::uint64_t field_revision=0,
    AdaptationPlanningCache* planning_cache=nullptr);

[[nodiscard]] AdaptationCommitResult replay_adaptation(
    TetMesh& mesh,const AdaptationReplayRecord& record,bool reverse,
    const AdaptationConfiguration& current_configuration,
    std::uint64_t current_field_revision=0);

struct Triangle { Vec3 a; Vec3 b; Vec3 c; };

// Surface-only strategies expose triangles without claiming volume coverage,
// cutaway connectivity, or exportable tetrahedra. The caller owns the triangle
// storage and must discard this view after the source hierarchy changes.
class SurfaceOnlyView {
 public:
  SurfaceOnlyView(const TetMesh& mesh,std::span<const Triangle> triangles) noexcept
      : mesh_(&mesh),triangles_(triangles),hierarchy_revision_(mesh.revision()) {}
  [[nodiscard]] std::span<const Triangle> triangles() const {
    if(!current())throw std::logic_error("surface-only view is stale");
    return triangles_;
  }
  [[nodiscard]] std::uint64_t hierarchy_revision() const noexcept{return hierarchy_revision_;}
  [[nodiscard]] bool current() const noexcept{
    return mesh_!=nullptr&&mesh_->revision()==hierarchy_revision_;
  }
 private:
  const TetMesh* mesh_{};
  std::span<const Triangle> triangles_;
  std::uint64_t hierarchy_revision_{};
};

[[nodiscard]] std::vector<Triangle> extract_isosurface(const TetMesh& mesh, const Sphere& sphere);
[[nodiscard]] std::vector<Triangle> extract_isosurface(
    const TetMesh& mesh,const Sphere& sphere,std::span<const TetId> tetrahedra);
// Tetrahedral dual contouring: one constrained QEF vertex per sign-changing
// active leaf, connected into polygons around sign-changing primal edges.
[[nodiscard]] std::vector<Triangle> extract_dual_contour(const TetMesh& mesh, const Sphere& sphere);

enum class SurfaceRelation { inside, outside, intersecting };

// Conservative classification: a tetrahedron may be reported as intersecting
// when the field cannot prove a uniform sign, but an actual implicit-surface
// intersection is never reported as inside or outside.
[[nodiscard]] SurfaceRelation classify_tetrahedron(const TetMesh& mesh, TetId tet, const Sphere& sphere);

}  // namespace tetra
