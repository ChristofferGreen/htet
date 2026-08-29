#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
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

// Keep the already-qualified renderer selectable until the faithful path has
// passed every Gate H oracle and performance gate.  The default changes only
// at the atomic H9 promotion.
enum class AtmosphereTransport {
  qualified_baseline,
  faithful_hillaire,
};

inline constexpr AtmosphereTransport default_atmosphere_transport=
    AtmosphereTransport::qualified_baseline;

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
};

[[nodiscard]] constexpr AtmosphereQualitySettings atmosphere_quality_settings(
    AtmosphereQuality quality) noexcept {
  if(quality==AtmosphereQuality::low)
    return {128U,32U,16U,384U,216U,16U,16U,8U,16U,8U,96U,54U,512U};
  if(quality==AtmosphereQuality::high)
    return {512U,128U,64U,768U,432U,64U,64U,32U,64U,32U,192U,108U,2048U};
  return {256U,64U,32U,384U,216U,32U,32U,16U,32U,16U,96U,54U,1024U};
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
[[nodiscard]] double atmosphere_shadow_filter_visibility(
    std::size_t lit_samples, std::size_t sample_count,
    double footprint_fade) noexcept;

int run_atmosphere_check(AtmospherePreset preset, double camera_altitude_metres,
                         double view_zenith_degrees,
                         double sun_zenith_degrees, std::ostream& output,
                         std::ostream& errors);

}  // namespace tetra_viewer
