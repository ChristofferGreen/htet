#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace tetra_viewer {

using AtmosphereSpectrum = std::array<double, 3>;

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

struct AtmosphereQualitySettings {
  unsigned transmittance_width{};
  unsigned transmittance_height{};
  unsigned multiple_scattering_size{};
  unsigned sky_width{};
  unsigned sky_height{};
  unsigned aerial_width{};
  unsigned aerial_height{};
  unsigned aerial_depth{};
  unsigned shadow_resolution{};
};

[[nodiscard]] constexpr AtmosphereQualitySettings atmosphere_quality_settings(
    AtmosphereQuality quality) noexcept {
  if(quality==AtmosphereQuality::low)
    return {128U,32U,16U,192U,108U,16U,16U,8U,512U};
  if(quality==AtmosphereQuality::high)
    return {512U,128U,64U,768U,432U,64U,64U,32U,2048U};
  return {256U,64U,32U,384U,216U,32U,32U,16U,1024U};
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
  AtmosphereSpectrum mie_absorption_per_metre{4.40e-6, 4.40e-6, 4.40e-6};
  double mie_scale_height_metres{1'200.0};
  double mie_anisotropy{0.8};
  AtmosphereSpectrum absorption_per_metre{0.650e-6, 1.881e-6, 0.085e-6};
  double absorption_peak_altitude_metres{25'000.0};
  double absorption_half_width_metres{15'000.0};
  AtmosphereSpectrum ground_albedo{0.10, 0.10, 0.10};
  AtmosphereSpectrum solar_irradiance{1.0, 1.0, 1.0};
  double solar_angular_radius_radians{0.004675};
};

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
// Cubic aerial-volume depth distribution: resolves local paths without giving
// up the finite ground-to-space extent or changing the physical atmosphere.
[[nodiscard]] double aerial_lut_distance(double slice,
                                         double maximum_distance_metres) noexcept;
[[nodiscard]] double aerial_lut_slice(double distance_metres,
                                      double maximum_distance_metres) noexcept;

int run_atmosphere_check(AtmospherePreset preset, double camera_altitude_metres,
                         double view_zenith_degrees,
                         double sun_zenith_degrees, std::ostream& output,
                         std::ostream& errors);

}  // namespace tetra_viewer
