#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tetra_viewer {

using AtmosphereSpectrum = std::array<double, 3>;
struct AtmosphereParameters;

enum class AtmospherePreset {
  gameplay_planet,
  earth,
  mars_like,
  dense_haze,
  nearly_airless,
  custom,
};

inline constexpr AtmospherePreset default_world_atmosphere_preset=
    AtmospherePreset::gameplay_planet;
inline constexpr double default_world_aerial_distance_metres=200'000.0;

enum class AtmosphereQuality {
  low,
  standard,
  high
};

// P9's renderer-facing quality controller owns only a discrete, already
// qualified raster-profile ladder. It deliberately has no atmospheric or
// shadow quality knob: physical sampling and occlusion remain invariant while
// a renderer decides whether a completed frame has enough headroom to change
// its internal raster scale.
struct MetalRasterQualityProfile {
  float render_scale{};
  std::uint32_t terrain_samples{};
};

enum class MetalQualityFrameClass { steady, moving };
enum class MetalQualityChange { none, upgrade, downgrade };

struct MetalQualityDecision {
  std::size_t profile_index{};
  MetalQualityChange change{MetalQualityChange::none};
  double percentile_95_milliseconds{};
  std::size_t dwell_frames{};
};

class MetalQualityController {
 public:
  explicit MetalQualityController(
      std::vector<MetalRasterQualityProfile> profiles,
      std::size_t initial_profile=0U,double target_milliseconds=1000.0/60.0);

  // Maintenance frames (lookup refreshes, uploads, and AS builds) are
  // deliberately classified out instead of causing a quality reaction.
  [[nodiscard]] MetalQualityDecision observe(double gpu_milliseconds,
      MetalQualityFrameClass frame_class,bool maintenance_frame=false);
  void set_target_milliseconds(double target_milliseconds) noexcept;
  [[nodiscard]] const MetalRasterQualityProfile& profile() const noexcept;
  [[nodiscard]] std::size_t profile_index() const noexcept;

 private:
  std::vector<MetalRasterQualityProfile> profiles_;
  std::array<std::vector<double>,2U> samples_;
  std::size_t profile_index_{};
  std::size_t dwell_frames_{};
  double target_milliseconds_{};
};

// Keep earlier transports selectable so reference-path regressions can be
// compared directly without reverting renderer work.
enum class AtmosphereTransport {
  qualified_baseline,
  faithful_hillaire,
  reference_hillaire_2020,
};

enum class AtmosphereRenderingMethod {
  current_qualified,
  native_screen_oracle,
  deterministic_half_resolution,
  temporal_half_resolution,
  deterministic_shadowed_froxels,
};

// The transport implementation has one visibility input, but the producer of
// that input is selected at runtime.  Keeping the choice separate from the
// sampling preset prevents an iOS quality toggle from silently changing the
// physical visibility algorithm.
enum class AtmosphereVisibilityBackend { automatic, ray_traced, fitted_minmax };

[[nodiscard]] std::optional<AtmosphereVisibilityBackend>
parse_atmosphere_visibility_backend(std::string_view name);
[[nodiscard]] std::string_view atmosphere_visibility_backend_name(
    AtmosphereVisibilityBackend backend) noexcept;

struct AtmosphereVisibilitySettings {
  AtmosphereVisibilityBackend requested{AtmosphereVisibilityBackend::automatic};
  bool ios_performance_mode{};
};

struct AtmosphereVisibilityPlan {
  AtmosphereVisibilityBackend effective{AtmosphereVisibilityBackend::fitted_minmax};
  std::uint32_t screen_divisor{2U};
  std::uint32_t rotating_queries_per_pixel{2U};
  bool requested_backend_available{};
};

// A forced ray-traced request remains visibly unavailable until both hardware
// and the native backend are ready; it is never reported as active while the
// fitted/min-max compatibility path is supplying the result.
[[nodiscard]] constexpr AtmosphereVisibilityPlan
resolve_atmosphere_visibility_plan(AtmosphereVisibilitySettings settings,
                                   bool ray_tracing_backend_available) noexcept {
  const bool wants_ray=settings.requested==AtmosphereVisibilityBackend::ray_traced;
  const bool automatic_ray=settings.requested==AtmosphereVisibilityBackend::automatic&&
      ray_tracing_backend_available;
  const bool ray_active=(wants_ray||automatic_ray)&&ray_tracing_backend_available;
  return {.effective=ray_active?AtmosphereVisibilityBackend::ray_traced:
                                AtmosphereVisibilityBackend::fitted_minmax,
          // Desktop RT integrates one physical atmosphere ray per native
          // pixel so terrain silhouettes never fall back to a second 2D
          // visibility representation. The explicit iOS-style preset keeps
          // its quarter-resolution budget and class-aware reconstruction.
          .screen_divisor=settings.ios_performance_mode?4U:(ray_active?1U:2U),
          .rotating_queries_per_pixel=settings.ios_performance_mode?1U:2U,
          .requested_backend_available=!wants_ray||ray_tracing_backend_available};
}

inline constexpr AtmosphereRenderingMethod default_atmosphere_rendering_method=
    AtmosphereRenderingMethod::current_qualified;
[[nodiscard]] std::optional<AtmosphereRenderingMethod>
parse_atmosphere_rendering_method(std::string_view name);
[[nodiscard]] std::string_view atmosphere_rendering_method_name(
    AtmosphereRenderingMethod method) noexcept;

enum class AtmosphereShadowIntegrator {
  fixed_32,
  adaptive_transition,
  minmax_segments,
  dense_oracle,
  moment_hybrid,
  epipolar_minmax,
};

enum class SurfaceShadowBiasMode { slope_scaled, receiver_plane };
[[nodiscard]] std::optional<SurfaceShadowBiasMode>
parse_surface_shadow_bias_mode(std::string_view name);
[[nodiscard]] std::string_view surface_shadow_bias_mode_name(
    SurfaceShadowBiasMode mode) noexcept;

enum class AtmosphereShadowFilter { unfiltered, fixed_tent, physical_footprint };
inline constexpr AtmosphereShadowFilter default_atmosphere_shadow_filter=
    AtmosphereShadowFilter::physical_footprint;
[[nodiscard]] std::optional<AtmosphereShadowFilter>
parse_atmosphere_shadow_filter(std::string_view name);
[[nodiscard]] std::string_view atmosphere_shadow_filter_name(
    AtmosphereShadowFilter filter) noexcept;

[[nodiscard]] std::uint64_t atmosphere_epipolar_source_revision(
    std::uint64_t surface_generation,const std::array<float,16>& matrix,
    float raster_bias_constant,float raster_bias_slope,
    AtmosphereShadowFilter filter,double comparison_bias_world) noexcept;
[[nodiscard]] std::uint64_t atmosphere_shadow_lookup_revision(
    std::uint64_t surface_generation,bool initialized,
    const std::array<float,16>& matrix) noexcept;

inline constexpr AtmosphereShadowIntegrator default_atmosphere_shadow_integrator=
    AtmosphereShadowIntegrator::fixed_32;

[[nodiscard]] std::optional<AtmosphereShadowIntegrator>
parse_atmosphere_shadow_integrator(std::string_view name);
[[nodiscard]] std::string_view atmosphere_shadow_integrator_name(
    AtmosphereShadowIntegrator integrator) noexcept;
[[nodiscard]] constexpr unsigned atmosphere_shadow_integrator_shader_index(
    AtmosphereShadowIntegrator integrator) noexcept {
  switch(integrator){
    case AtmosphereShadowIntegrator::fixed_32:return 0U;
    case AtmosphereShadowIntegrator::adaptive_transition:return 1U;
    case AtmosphereShadowIntegrator::minmax_segments:return 2U;
    case AtmosphereShadowIntegrator::dense_oracle:return 3U;
    case AtmosphereShadowIntegrator::moment_hybrid:return 4U;
    case AtmosphereShadowIntegrator::epipolar_minmax:return 5U;
  }
  return 0U;
}

struct AtmosphereShadowIntegrationOptions {
  std::size_t base_intervals{32U};
  std::size_t dense_intervals{1024U};
  std::size_t maximum_subdivision_depth{5U};
  std::size_t maximum_samples{2048U};
  double transition_tolerance{1.0e-6};
};

struct AtmosphereShadowInterval {
  double begin{};
  double end{};
  double visibility{1.0};
};

struct AtmosphereShadowIntegrationResult {
  double weighted_loss{};
  double total_weight{};
  double maximum_loss{};
  std::size_t visibility_samples{};
  std::size_t visited_ranges{};
  std::vector<AtmosphereShadowInterval> intervals;
  bool fallback{};
};

struct AtmosphereShadowIntegrationGeneration {
  AtmosphereShadowIntegrator integrator{default_atmosphere_shadow_integrator};
  std::uint64_t shadow_depth_generation{};
  std::uint64_t hierarchy_generation{};
  bool fallback{};
  bool complete{};
};

[[nodiscard]] bool compatible_atmosphere_shadow_generation(
    const AtmosphereShadowIntegrationGeneration& generation) noexcept;

using AtmosphereShadowVisibilityFunction=std::function<double(double)>;
using AtmosphereShadowWeightFunction=std::function<double(double)>;
// A range classifier returns a constant visibility only when the complete
// interval is proven uniform. A null result asks the integrator to descend.
using AtmosphereShadowRangeClassifier=
    std::function<std::optional<double>(double,double)>;

[[nodiscard]] AtmosphereShadowIntegrationResult integrate_atmosphere_shadow(
    AtmosphereShadowIntegrator integrator,
    const AtmosphereShadowVisibilityFunction& visibility,
    const AtmosphereShadowWeightFunction& weight={},
    const AtmosphereShadowRangeClassifier& classify_range={},
    AtmosphereShadowIntegrationOptions options={});

struct AtmosphereShadowErrorMetrics {
  double root_mean_square_error{};
  double maximum_error{};
  double boundary_distance{};
  double gradient_step_energy{};
};

struct AtmosphereShadowDepthRange {
  double minimum{1.0};
  double maximum{1.0};
};

struct AtmosphereShadowDepthLevel {
  std::size_t width{};
  std::size_t height{};
  std::vector<AtmosphereShadowDepthRange> ranges;
};

struct AtmosphereShadowDepthHierarchy {
  std::vector<AtmosphereShadowDepthLevel> levels;
};

struct AtmosphereEpipolarDepthLevel {
  std::size_t width{};
  std::size_t rows{};
  std::vector<AtmosphereShadowDepthRange> ranges;
};

struct AtmosphereEpipolarDepthHierarchy {
  std::size_t radial_resolution{};
  std::size_t angular_rows{};
  std::vector<AtmosphereEpipolarDepthLevel> levels;
};

struct AtmosphereEpipolarTraversalResult {
  std::vector<AtmosphereShadowInterval> intervals;
  std::size_t visited_nodes{};
  bool fallback{};
};

struct AtmosphereMomentVisibility {
  double lower{};
  double upper{1.0};
  double estimate{1.0};
  double residual{1.0};
  bool confident{};
};

[[nodiscard]] AtmosphereMomentVisibility
atmosphere_moment_visibility(std::span<const double> blocker_depths,
                             double receiver_depth,
                             double residual_tolerance=1.0e-5);

[[nodiscard]] AtmosphereShadowDepthHierarchy
make_atmosphere_shadow_depth_hierarchy(std::span<const double> depth,
                                       std::size_t width,
                                       std::size_t height,
                                       double clear_depth=1.0);

[[nodiscard]] AtmosphereEpipolarDepthHierarchy
make_atmosphere_epipolar_depth_hierarchy(std::span<const double> depth,
                                         std::size_t radial_resolution,
                                         std::size_t angular_rows,
                                         double clear_depth=1.0);
[[nodiscard]] AtmosphereEpipolarTraversalResult
traverse_atmosphere_epipolar_row(
    const AtmosphereEpipolarDepthHierarchy& hierarchy,std::size_t row,
    double radial_begin,double radial_end,double receiver_depth_begin,
    double receiver_depth_end,double comparison_bias=0.0,
    std::size_t maximum_intervals=128U);

[[nodiscard]] AtmosphereShadowErrorMetrics atmosphere_shadow_error_metrics(
    std::span<const double> candidate,std::span<const double> reference,
    double sample_spacing=1.0);

// A rectified epipolar hierarchy stores one independent one-dimensional
// min/max pyramid for every angular row.  Keeping this sizing rule in the CPU
// API makes allocation, diagnostics, and tests agree with the shader layout.
[[nodiscard]] std::size_t atmosphere_epipolar_minmax_element_count(
    std::size_t radial_resolution,std::size_t angular_rows) noexcept;
struct AtmosphereEpipolarLayout {
  std::size_t radial_resolution{};
  std::size_t angular_rows{};
  std::size_t element_count{};
};
[[nodiscard]] AtmosphereEpipolarLayout atmosphere_epipolar_layout(
    std::size_t shadow_resolution) noexcept;
[[nodiscard]] std::size_t atmosphere_epipolar_row_from_angle(
    double angle_radians,std::size_t angular_rows) noexcept;

inline constexpr AtmosphereTransport default_atmosphere_transport=
    AtmosphereTransport::reference_hillaire_2020;

[[nodiscard]] std::optional<AtmosphereTransport> parse_atmosphere_transport(
    std::string_view name);
[[nodiscard]] std::string_view atmosphere_transport_name(
    AtmosphereTransport transport) noexcept;

template<class Tag>
struct AtmosphereRevision {
  std::uint64_t value{};
  friend constexpr bool operator==(AtmosphereRevision,
                                   AtmosphereRevision) noexcept = default;
};

struct AtmosphereOpticalRevisionTag;
struct AtmosphereScatteringRevisionTag;
struct AtmosphereSunRevisionTag;
struct AtmosphereCameraPositionRevisionTag;
struct AtmosphereSkyPositionRevisionTag;
struct AtmosphereCameraOrientationRevisionTag;
struct AtmosphereShadowIntegratorRevisionTag;
struct AtmosphereShadowRevisionTag;
struct AtmosphereRenderOriginRevisionTag;

using AtmosphereOpticalRevision=
    AtmosphereRevision<AtmosphereOpticalRevisionTag>;
using AtmosphereScatteringRevision=
    AtmosphereRevision<AtmosphereScatteringRevisionTag>;
using AtmosphereSunRevision=AtmosphereRevision<AtmosphereSunRevisionTag>;
using AtmosphereCameraPositionRevision=
    AtmosphereRevision<AtmosphereCameraPositionRevisionTag>;
using AtmosphereSkyPositionRevision=
    AtmosphereRevision<AtmosphereSkyPositionRevisionTag>;
using AtmosphereCameraOrientationRevision=
    AtmosphereRevision<AtmosphereCameraOrientationRevisionTag>;
using AtmosphereShadowIntegratorRevision=
    AtmosphereRevision<AtmosphereShadowIntegratorRevisionTag>;
using AtmosphereShadowRevision=AtmosphereRevision<AtmosphereShadowRevisionTag>;
using AtmosphereRenderOriginRevision=
    AtmosphereRevision<AtmosphereRenderOriginRevisionTag>;

struct AtmosphereLookupRevisions {
  AtmosphereOpticalRevision optical{};
  AtmosphereScatteringRevision scattering{};
  AtmosphereSunRevision sun{};
  AtmosphereCameraPositionRevision camera_position{};
  AtmosphereSkyPositionRevision sky_position{};
  AtmosphereCameraOrientationRevision camera_orientation{};
  AtmosphereShadowIntegratorRevision shadow_integrator{};
  AtmosphereShadowRevision shadow{};
  AtmosphereRenderOriginRevision render_origin{};
  friend constexpr bool operator==(const AtmosphereLookupRevisions&,
                                   const AtmosphereLookupRevisions&) noexcept =
      default;
};

struct AtmosphereDispatchPlan {
  bool transmittance{};
  bool multiple_scattering{};
  bool sky_view{};
  bool sky_irradiance{};
  bool aerial_perspective{};
  bool long_shadow{};
};

// This is the authoritative lookup dependency model.  The baseline sky image
// is view-frustum data and therefore follows orientation; the faithful path is
// a local-up/sun full-sky table and deliberately does not.
[[nodiscard]] AtmosphereDispatchPlan atmosphere_dispatch_plan(
    std::optional<AtmosphereLookupRevisions> previous,
    const AtmosphereLookupRevisions& next,
    AtmosphereTransport transport) noexcept;
[[nodiscard]] std::uint64_t atmosphere_optical_hash(
    const AtmosphereParameters& parameters);
[[nodiscard]] std::uint64_t atmosphere_scattering_hash(
    const AtmosphereParameters& parameters);
[[nodiscard]] AtmosphereSkyPositionRevision atmosphere_sky_position_revision(
    tetra::Vec3 position_from_planet_centre_metres,
    const AtmosphereParameters& parameters) noexcept;

struct AtmosphereQualitySettings {
  unsigned transmittance_width{};
  unsigned transmittance_height{};
  unsigned multiple_scattering_size{};
  unsigned sky_width{};
  unsigned sky_height{};
  unsigned aerial_width{};
  unsigned aerial_height{};
  unsigned aerial_depth{};
  unsigned irradiance_width{};
  unsigned irradiance_height{};
  unsigned long_shadow_width{};
  unsigned long_shadow_height{};
  unsigned shadow_resolution{};
  unsigned atmosphere_shadow_resolution{};
};

[[nodiscard]] constexpr AtmosphereQualitySettings atmosphere_quality_settings(
    AtmosphereQuality quality) noexcept {
  if(quality==AtmosphereQuality::low)
    return {128U,32U,16U,384U,216U,96U,54U,8U,16U,8U,192U,108U,512U,256U};
  if(quality==AtmosphereQuality::high)
    return {512U,128U,64U,768U,432U,384U,216U,32U,64U,32U,1024U,576U,2048U,1024U};
  return {256U,64U,32U,384U,216U,192U,108U,16U,32U,16U,768U,432U,1024U,512U};
}

struct AtmosphereParameters {
  double ground_radius_metres{6'360'000.0};
  double atmosphere_height_metres{100'000.0};
  double metres_per_world_unit{1.0};
  AtmosphereSpectrum rayleigh_scattering_per_metre{
      5.802e-6, 13.558e-6, 33.100e-6};
  double rayleigh_scale_height_metres{8'000.0};
  AtmosphereSpectrum mie_scattering_per_metre{3.996e-6, 3.996e-6,
                                               3.996e-6};
  // Bruneton/Hillaire's commonly quoted 4.4e-6 value is Mie extinction.
  // Absorption is extinction minus the 3.996e-6 scattering coefficient.
  AtmosphereSpectrum mie_absorption_per_metre{0.404e-6, 0.404e-6,
                                               0.404e-6};
  double mie_scale_height_metres{1'200.0};
  double mie_anisotropy{0.8};
  AtmosphereSpectrum absorption_per_metre{0.650e-6, 1.881e-6, 0.085e-6};
  double absorption_peak_altitude_metres{25'000.0};
  double absorption_half_width_metres{15'000.0};
  AtmosphereSpectrum ground_albedo{0.10, 0.10, 0.10};
  AtmosphereSpectrum solar_irradiance{1.0, 1.0, 1.0};
  double solar_angular_radius_radians{0.004675};
};

// These are whole-value generation records. A renderer constructs a new value
// after dispatch and publishes it atomically; retained lookup resources never
// have their dependency identity edited field by field.
struct AtmosphereMaterialSnapshot {
  AtmosphereParameters parameters{};
  AtmosphereOpticalRevision optical{};
  AtmosphereScatteringRevision scattering{};
  AtmosphereTransport transport{default_atmosphere_transport};
};

struct AtmosphereOpticalLookupSnapshot {
  AtmosphereOpticalRevision optical{};
  AtmosphereTransport transport{default_atmosphere_transport};
};

struct AtmosphereLightingLookupSnapshot {
  AtmosphereOpticalRevision optical{};
  AtmosphereScatteringRevision scattering{};
  AtmosphereSunRevision sun{};
  AtmosphereSkyPositionRevision sky_position{};
  AtmosphereCameraOrientationRevision camera_orientation{};
  AtmosphereTransport transport{default_atmosphere_transport};
};

struct AtmosphereViewLookupSnapshot {
  AtmosphereOpticalRevision optical{};
  AtmosphereScatteringRevision scattering{};
  AtmosphereSunRevision sun{};
  AtmosphereCameraPositionRevision camera_position{};
  AtmosphereCameraOrientationRevision camera_orientation{};
  AtmosphereShadowIntegratorRevision shadow_integrator{};
  AtmosphereShadowRevision shadow{};
  AtmosphereRenderOriginRevision render_origin{};
  AtmosphereTransport transport{default_atmosphere_transport};
};

struct AtmosphereLookupSnapshotSet {
  std::optional<AtmosphereOpticalLookupSnapshot> optical;
  std::optional<AtmosphereLightingLookupSnapshot> lighting;
  std::optional<AtmosphereViewLookupSnapshot> view;
};

struct AtmosphereValidationSnapshot {
  AtmosphereMaterialSnapshot material{};
  AtmosphereLookupSnapshotSet lookups{};
  AtmosphereLookupRevisions frame{};
  std::optional<std::string> incompatibility;
  [[nodiscard]] bool compatible() const noexcept {
    return !incompatibility.has_value();
  }
};

[[nodiscard]] AtmosphereMaterialSnapshot atmosphere_material_snapshot(
    const AtmosphereParameters& parameters,AtmosphereTransport transport);
[[nodiscard]] AtmosphereLookupSnapshotSet advance_atmosphere_lookup_snapshots(
    std::optional<AtmosphereLookupSnapshotSet> previous,
    const AtmosphereMaterialSnapshot& material,
    const AtmosphereLookupRevisions& next,
    const AtmosphereDispatchPlan& dispatch);
[[nodiscard]] AtmosphereValidationSnapshot atmosphere_validation_snapshot(
    const AtmosphereMaterialSnapshot& material,
    const AtmosphereLookupSnapshotSet& lookups,
    const AtmosphereLookupRevisions& frame);

struct AtmosphereInvalidation {
  bool transmittance{};
  bool multiple_scattering{};
  bool sky_view{};
  bool aerial_perspective{};
};

struct AtmosphereRaySegment {
  double begin_metres{};
  double end_metres{};
};

struct AtmosphereOpticalDepth {
  double rayleigh{};
  double mie{};
  double absorption{};
};

struct AtmosphereLookupCoordinates {
  double u{};
  double v{};
};

struct AtmosphereTransmittanceParameters {
  double altitude_metres{};
  double zenith_cosine{1.0};
};

struct AtmosphereMultipleScatteringReference {
  AtmosphereSpectrum second_order{};
  AtmosphereSpectrum transfer_factor{};
  AtmosphereSpectrum closed_contribution{};
};

struct AtmosphereScatteringReference {
  AtmosphereSpectrum radiance{};
  AtmosphereSpectrum transmittance{1.0,1.0,1.0};
};

// Split reference transport used to qualify every screen and froxel
// representation. Terrain visibility attenuates direct single scattering at
// the sample that creates it; it never subtracts already integrated radiance
// and it does not shadow the current low-frequency multiple-scattering model.
struct AtmosphereScatteringComponents {
  AtmosphereSpectrum direct_single_scattering{};
  AtmosphereSpectrum multiple_scattering{};
  AtmosphereSpectrum transmittance{1.0,1.0,1.0};
};

using AtmosphereTerrainVisibilityFunction=
    std::function<double(tetra::Vec3 position_from_planet_centre_metres)>;

enum class AtmosphereEndpointClass : std::uint8_t { sky, opaque };

struct AtmosphereReducedEndpoint {
  AtmosphereEndpointClass classification{AtmosphereEndpointClass::sky};
  float reversed_depth{};
  double linear_depth_metres{};
  double transition_confidence{1.0};
  std::uint32_t generation{};
};

// A reduced endpoint stores this compact native coordinate in its class lane:
// zero denotes sky, and opaque offsets are one plus their 4x4 row-major index.
// The representation is deliberately independent of a particular reduction
// divisor, so every 1x--4x footprint uses the same GPU-safe 0--16 range.
struct AtmosphereEndpointNativeOffset {
  AtmosphereEndpointClass classification{AtmosphereEndpointClass::sky};
  std::uint32_t x{};
  std::uint32_t y{};
};

[[nodiscard]] std::optional<std::uint32_t>
pack_atmosphere_endpoint_native_offset(
    AtmosphereEndpointClass classification,std::uint32_t divisor,
    std::uint32_t x=0U,std::uint32_t y=0U) noexcept;

[[nodiscard]] std::optional<AtmosphereEndpointNativeOffset>
unpack_atmosphere_endpoint_native_offset(
    std::uint32_t packed,std::uint32_t divisor) noexcept;

// Conservatively represents a 2x2 native footprint with its nearest opaque
// endpoint. An all-clear footprint remains sky. Invalid depth is rejected so
// it cannot enter temporal history as apparently valid geometry.
[[nodiscard]] std::optional<AtmosphereReducedEndpoint>
reduce_atmosphere_endpoint_2x2(
    const std::array<float,4>& reversed_depth,
    double near_plane_metres,std::uint32_t generation=0U) noexcept;

[[nodiscard]] std::array<double,4> atmosphere_reconstruction_weights(
    AtmosphereEndpointClass target_class,double target_linear_depth_metres,
    const std::array<AtmosphereReducedEndpoint,4>& taps,
    const std::array<double,4>& bilinear_weights,
    double relative_depth_tolerance=0.05) noexcept;

enum class AtmosphereHistoryInvalidation : std::uint32_t {
  none=0U,
  uninitialized=1U<<0U,
  optical=1U<<1U,
  scattering=1U<<2U,
  sun=1U<<3U,
  shadow_integrator=1U<<4U,
  shadow=1U<<5U,
  terrain=1U<<6U,
  representation=1U<<7U,
  extent=1U<<8U
};

struct AtmosphereScreenHistoryIdentity {
  AtmosphereLookupRevisions revisions{};
  std::uint64_t terrain_generation{};
  std::uint64_t result_generation{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t linear_resolution_divisor{2U};
  std::uint32_t sample_count{32U};
  AtmosphereTransport transport{default_atmosphere_transport};
  AtmosphereRenderingMethod rendering_method{
      AtmosphereRenderingMethod::deterministic_half_resolution};
  bool valid{};
};

struct AtmosphereHistoryCompatibility {
  std::uint32_t invalidation_mask{};
  bool camera_changed{};
  bool render_origin_changed{};
  [[nodiscard]] constexpr bool compatible() const noexcept {
    return invalidation_mask==0U;
  }
};

// Cached visibility belongs to a specific camera ray. Unlike reconstructed
// radiance, it cannot be safely clamped after reprojection: a stale binary
// occlusion interval becomes a visible light shaft at the wrong screen
// position. Static views may amortize exact queries, but every reprojected
// view must refresh the complete interval set in the current frame.
[[nodiscard]] constexpr std::uint32_t
atmosphere_visibility_refresh_intervals(
    bool temporal,const AtmosphereHistoryCompatibility& compatibility) noexcept {
  return !temporal||!compatibility.compatible()||
                 compatibility.camera_changed||
                 compatibility.render_origin_changed?32U:2U;
}

// CPU mirror of the radial visibility reconstruction in atmosphere.comp.
// Constant runs are preserved exactly; only intervals adjacent to a sampled
// shadow transition are blended.
[[nodiscard]] constexpr double atmosphere_interval_visibility(
    double previous,double centre,double next) noexcept {
  return (previous+2.0*centre+next)*0.25;
}

// Camera motion and render-origin rebasing are explicitly reprojectable.
// Material, light, terrain/shadow, and representation changes are not: old
// radiance must never be interpreted as a sample of the new transport.
[[nodiscard]] AtmosphereHistoryCompatibility
atmosphere_screen_history_compatibility(
    const AtmosphereScreenHistoryIdentity& previous,
    const AtmosphereScreenHistoryIdentity& current) noexcept;

struct AtmosphereReprojectionCamera {
  tetra::Vec3 position_from_planet_centre_metres{};
  tetra::Vec3 right{1.0,0.0,0.0};
  tetra::Vec3 down{0.0,1.0,0.0};
  tetra::Vec3 forward{0.0,0.0,1.0};
  double tangent_x{1.0};
  double tangent_y{1.0};
};

struct AtmosphereReprojectedEndpoint {
  double u{};
  double v{};
  double previous_linear_depth_metres{};
};

[[nodiscard]] std::optional<AtmosphereReprojectedEndpoint>
reproject_atmosphere_endpoint(
    const AtmosphereReprojectionCamera& current,
    const AtmosphereReprojectionCamera& previous,double current_u,
    double current_v,AtmosphereEndpointClass classification,
    double current_linear_depth_metres) noexcept;

[[nodiscard]] bool atmosphere_history_sample_compatible(
    const AtmosphereReducedEndpoint& current,
    const AtmosphereReducedEndpoint& previous,
    double expected_previous_depth_metres,
    std::uint32_t expected_previous_generation,
    bool reprojection_inside_view,
    double relative_depth_tolerance=0.05) noexcept;

inline constexpr std::size_t atmosphere_boundary_probe_case_count=7U;
inline constexpr std::size_t atmosphere_numeric_probe_base_value_count=10U;
inline constexpr std::size_t atmosphere_numeric_probe_value_count=
    atmosphere_numeric_probe_base_value_count+
    atmosphere_boundary_probe_case_count*3U;
using AtmosphereNumericProbeValue=std::array<double,4>;
using AtmosphereNumericProbeValues=
    std::array<AtmosphereNumericProbeValue,atmosphere_numeric_probe_value_count>;

struct AtmosphereNumericProbeInput {
  AtmosphereParameters parameters{};
  tetra::Vec3 camera_position_from_planet_centre_metres{};
  tetra::Vec3 camera_right{1.0,0.0,0.0};
  tetra::Vec3 camera_down{0.0,1.0,0.0};
  tetra::Vec3 camera_forward{0.0,0.0,1.0};
  tetra::Vec3 sun_direction{0.0,1.0,0.0};
  double vertical_tangent{1.0};
  double aspect_ratio{1.0};
  double maximum_aerial_distance_metres{default_world_aerial_distance_metres};
  AtmosphereQuality quality{AtmosphereQuality::standard};
};

struct AtmosphereNumericProbeComparison {
  std::string name;
  AtmosphereNumericProbeValue actual{};
  AtmosphereNumericProbeValue expected{};
  AtmosphereNumericProbeValue absolute_error{};
  AtmosphereNumericProbeValue relative_error{};
  bool passed{};
};

struct AtmosphereNumericProbeReport {
  AtmosphereNumericProbeValues reference{};
  std::vector<AtmosphereNumericProbeComparison> comparisons;
  bool passed{};
};

using AtmosphereSkyRadianceFunction=
    std::function<AtmosphereSpectrum(tetra::Vec3 direction)>;

[[nodiscard]] AtmosphereParameters atmosphere_preset(AtmospherePreset preset);
// Expands the compact gameplay atmosphere around a known terrain envelope.
// Molecular coefficients are rescaled inversely with scale height so the
// vertical optical depth, and therefore the ground-level calibration, stays
// constant while the visible limb has room above high relief.
[[nodiscard]] AtmosphereParameters adapt_compact_atmosphere_to_relief(
    AtmosphereParameters parameters,double maximum_relief_metres) noexcept;
[[nodiscard]] std::optional<AtmospherePreset> parse_atmosphere_preset(
    std::string_view name);
[[nodiscard]] std::string_view atmosphere_preset_name(AtmospherePreset preset);
[[nodiscard]] std::optional<std::string> validate_atmosphere(
    const AtmosphereParameters& parameters);
[[nodiscard]] std::uint64_t atmosphere_parameter_hash(
    const AtmosphereParameters& parameters);
[[nodiscard]] std::string serialize_atmosphere_parameters(
    const AtmosphereParameters& parameters);
[[nodiscard]] AtmosphereInvalidation atmosphere_invalidation(
    const AtmosphereParameters& before, const AtmosphereParameters& after);

[[nodiscard]] std::optional<AtmosphereRaySegment> atmosphere_ray_segment(
    tetra::Vec3 position_from_planet_centre_metres,
    tetra::Vec3 direction,
    const AtmosphereParameters& parameters);
[[nodiscard]] double atmosphere_rayleigh_density(
    double altitude_metres, const AtmosphereParameters& parameters);
[[nodiscard]] double atmosphere_mie_density(
    double altitude_metres, const AtmosphereParameters& parameters);
[[nodiscard]] double atmosphere_absorption_density(
    double altitude_metres, const AtmosphereParameters& parameters);
[[nodiscard]] double rayleigh_phase(double cosine_angle);
[[nodiscard]] double mie_henyey_greenstein_phase(double cosine_angle,
                                                  double anisotropy);
[[nodiscard]] AtmosphereOpticalDepth atmosphere_optical_depth(
    tetra::Vec3 start_from_planet_centre_metres,
    tetra::Vec3 end_from_planet_centre_metres,
    const AtmosphereParameters& parameters,
    std::size_t integration_steps = 128U);
[[nodiscard]] AtmosphereSpectrum atmosphere_transmittance(
    tetra::Vec3 start_from_planet_centre_metres,
    tetra::Vec3 end_from_planet_centre_metres,
    const AtmosphereParameters& parameters,
    std::size_t integration_steps = 128U);
[[nodiscard]] AtmosphereLookupCoordinates atmosphere_transmittance_uv(
    double altitude_metres, double zenith_cosine,
    const AtmosphereParameters& parameters) noexcept;
[[nodiscard]] AtmosphereTransmittanceParameters
atmosphere_transmittance_parameters(
    AtmosphereLookupCoordinates uv,
    const AtmosphereParameters& parameters) noexcept;
[[nodiscard]] AtmosphereLookupCoordinates atmosphere_full_sky_uv(
    tetra::Vec3 direction, tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept;
[[nodiscard]] tetra::Vec3 atmosphere_full_sky_direction(
    AtmosphereLookupCoordinates uv, tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept;
[[nodiscard]] AtmosphereLookupCoordinates atmosphere_sun_focused_sky_uv(
    tetra::Vec3 direction, tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept;
[[nodiscard]] tetra::Vec3 atmosphere_sun_focused_sky_direction(
    AtmosphereLookupCoordinates uv, tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept;
[[nodiscard]] AtmosphereLookupCoordinates atmosphere_sun_shadow_sky_uv(
    tetra::Vec3 direction, tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept;
[[nodiscard]] tetra::Vec3 atmosphere_sun_shadow_sky_direction(
    AtmosphereLookupCoordinates uv, tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept;
[[nodiscard]] AtmosphereSpectrum atmosphere_multiple_scattering_closure(
    const AtmosphereSpectrum& second_order,
    const AtmosphereSpectrum& transfer_factor) noexcept;
[[nodiscard]] AtmosphereMultipleScatteringReference
atmosphere_multiple_scattering_reference(
    const AtmosphereParameters& parameters, double altitude_metres,
    double sun_zenith_cosine, std::size_t direction_count=64U,
    std::size_t ray_steps=20U);
[[nodiscard]] AtmosphereScatteringReference atmosphere_scattering_reference(
    const AtmosphereParameters& parameters,
    tetra::Vec3 position_from_planet_centre_metres,
    tetra::Vec3 view_direction, tetra::Vec3 sun_direction,
    double maximum_distance_metres,
    std::size_t view_steps=32U,
    std::size_t multiple_direction_count=16U,
    std::size_t multiple_ray_steps=8U);
[[nodiscard]] AtmosphereScatteringComponents
atmosphere_scattering_components_reference(
    const AtmosphereParameters& parameters,
    tetra::Vec3 position_from_planet_centre_metres,
    tetra::Vec3 view_direction, tetra::Vec3 sun_direction,
    double maximum_distance_metres,
    const AtmosphereTerrainVisibilityFunction& terrain_visibility,
    std::size_t view_steps=32U,
    std::size_t multiple_direction_count=16U,
    std::size_t multiple_ray_steps=8U);
[[nodiscard]] AtmosphereNumericProbeValues atmosphere_numeric_probe_reference(
    const AtmosphereNumericProbeInput& input);
[[nodiscard]] AtmosphereNumericProbeReport evaluate_atmosphere_numeric_probe(
    const AtmosphereNumericProbeValues& actual,
    const AtmosphereNumericProbeInput& input);
// Independent double-precision cosine convolution used to qualify the GPU
// irradiance lookup. The returned value is E/pi, ready for Lambertian albedo.
[[nodiscard]] AtmosphereSpectrum atmosphere_sky_irradiance_reference(
    tetra::Vec3 surface_normal, tetra::Vec3 local_up,
    const AtmosphereSkyRadianceFunction& sky_radiance,
    std::size_t direction_count=4096U);
// Cubic aerial-volume depth distribution: resolves local paths without giving
// up the finite ground-to-space extent or changing the physical atmosphere.
[[nodiscard]] double aerial_lut_distance(double slice,
                                         double maximum_distance_metres) noexcept;
[[nodiscard]] double aerial_lut_slice(double distance_metres,
                                      double maximum_distance_metres) noexcept;
[[nodiscard]] double atmosphere_local_aerial_distance(
    const AtmosphereParameters& parameters, double camera_altitude_metres,
    double visible_distance_metres) noexcept;
// Terrain relief can place a perfectly valid player below the atmosphere's
// smooth reference ellipsoid.  Atmosphere lookups cannot use an observer
// inside that opaque boundary; project only the transport observer to a
// small positive altitude while scene geometry keeps its true position.
[[nodiscard]] tetra::Vec3 clamp_atmosphere_camera_to_medium(
    tetra::Vec3 position_from_planet_centre_metres,
    const AtmosphereParameters& parameters,
    double minimum_altitude_metres=1.0) noexcept;
[[nodiscard]] double atmosphere_shadow_filter_visibility(
    std::size_t lit_samples, std::size_t sample_count,
    double footprint_fade) noexcept;

int run_atmosphere_check(AtmospherePreset preset, double camera_altitude_metres,
                         double view_zenith_degrees,
                         double sun_zenith_degrees, std::ostream& output,
                         std::ostream& errors);

}  // namespace tetra_viewer
