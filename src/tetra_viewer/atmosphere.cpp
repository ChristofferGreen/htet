#include "tetra_viewer/atmosphere.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>
#include <utility>

namespace tetra_viewer {
namespace {

double length(tetra::Vec3 value) {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

tetra::Vec3 normalized(tetra::Vec3 value) {
  const double magnitude = length(value);
  if (!(magnitude > 0.0) || !std::isfinite(magnitude)) return {};
  return value / magnitude;
}

double dot(tetra::Vec3 first, tetra::Vec3 second) {
  return first.x * second.x + first.y * second.y + first.z * second.z;
}

tetra::Vec3 cross(tetra::Vec3 first,tetra::Vec3 second) {
  return {first.y*second.z-first.z*second.y,
          first.z*second.x-first.x*second.z,
          first.x*second.y-first.y*second.x};
}

struct SkyBasis {
  tetra::Vec3 up;
  tetra::Vec3 sun_tangent;
  tetra::Vec3 longitude_tangent;
};

SkyBasis sky_basis(tetra::Vec3 local_up,tetra::Vec3 sun_direction) {
  local_up=normalized(local_up);
  if(length(local_up)==0.0)local_up={0.0,1.0,0.0};
  sun_direction=normalized(sun_direction);
  tetra::Vec3 tangent=sun_direction-local_up*dot(sun_direction,local_up);
  if(length(tangent)<1.0e-12){
    const tetra::Vec3 reference=std::abs(local_up.z)<0.9?
        tetra::Vec3{0.0,0.0,1.0}:tetra::Vec3{1.0,0.0,0.0};
    tangent=reference-local_up*dot(reference,local_up);
  }
  tangent=normalized(tangent);
  return {local_up,tangent,normalized(cross(local_up,tangent))};
}

bool finite_spectrum(const AtmosphereSpectrum& value) {
  return std::ranges::all_of(value, [](double component) {
    return std::isfinite(component);
  });
}

bool nonnegative_spectrum(const AtmosphereSpectrum& value) {
  return std::ranges::all_of(value,
                             [](double component) { return component >= 0.0; });
}

bool equal(const AtmosphereSpectrum& first, const AtmosphereSpectrum& second) {
  return first == second;
}

void hash_double(std::uint64_t& hash, double value) {
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto bits = std::bit_cast<std::uint64_t>(value);
  for (unsigned int byte = 0; byte < 8U; ++byte) {
    hash ^= (bits >> (byte * 8U)) & 0xffU;
    hash *= prime;
  }
}

std::optional<std::array<double, 2>> sphere_roots(tetra::Vec3 origin,
                                                  tetra::Vec3 direction,
                                                  double radius) {
  const double b = dot(origin, direction);
  const double radial_distance = length(origin);
  const double c = (radial_distance - radius) *
                   (radial_distance + radius);
  double discriminant = b * b - c;
  const double tolerance = 32.0 * std::numeric_limits<double>::epsilon() *
                           std::max(b * b, std::abs(c));
  if (discriminant < -tolerance) return std::nullopt;
  discriminant = std::max(0.0, discriminant);
  const double root = std::sqrt(discriminant);
  return std::array<double, 2>{-b - root, -b + root};
}

bool extinction_changed(const AtmosphereParameters& first,
                         const AtmosphereParameters& second) {
  return first.ground_radius_metres != second.ground_radius_metres ||
         first.atmosphere_height_metres != second.atmosphere_height_metres ||
         first.metres_per_world_unit != second.metres_per_world_unit ||
         !equal(first.rayleigh_scattering_per_metre,
                second.rayleigh_scattering_per_metre) ||
         first.rayleigh_scale_height_metres !=
             second.rayleigh_scale_height_metres ||
         !equal(first.mie_scattering_per_metre,
                second.mie_scattering_per_metre) ||
         !equal(first.mie_absorption_per_metre,
                second.mie_absorption_per_metre) ||
         first.mie_scale_height_metres != second.mie_scale_height_metres ||
         !equal(first.absorption_per_metre, second.absorption_per_metre) ||
         first.absorption_peak_altitude_metres !=
             second.absorption_peak_altitude_metres ||
         first.absorption_half_width_metres !=
             second.absorption_half_width_metres;
}

}  // namespace

std::optional<AtmosphereTransport> parse_atmosphere_transport(
    std::string_view name) {
  if (name == "qualified-baseline")
    return AtmosphereTransport::qualified_baseline;
  if (name == "faithful-hillaire")
    return AtmosphereTransport::faithful_hillaire;
  return std::nullopt;
}

std::string_view atmosphere_transport_name(
    AtmosphereTransport transport) noexcept {
  switch (transport) {
    case AtmosphereTransport::qualified_baseline:
      return "qualified-baseline";
    case AtmosphereTransport::faithful_hillaire:
      return "faithful-hillaire";
  }
  return "qualified-baseline";
}

AtmosphereDispatchPlan atmosphere_dispatch_plan(
    std::optional<AtmosphereLookupRevisions> previous,
    const AtmosphereLookupRevisions& next,
    AtmosphereTransport transport) noexcept {
  if (!previous)
    return {.transmittance=true, .multiple_scattering=true, .sky_view=true,
            .sky_irradiance=true, .aerial_perspective=true,
            .long_shadow=transport==AtmosphereTransport::faithful_hillaire};

  const bool optical=previous->optical!=next.optical;
  const bool scattering=previous->scattering!=next.scattering;
  const bool sun=previous->sun!=next.sun;
  const bool position=previous->camera_position!=next.camera_position;
  const bool sky_position=previous->sky_position!=next.sky_position;
  const bool orientation=
      previous->camera_orientation!=next.camera_orientation;
  const bool shadow=previous->shadow!=next.shadow;
  const bool origin=previous->render_origin!=next.render_origin;
  const bool baseline=transport==AtmosphereTransport::qualified_baseline;
  return {
      .transmittance=optical,
      .multiple_scattering=optical||scattering,
      .sky_view=optical||scattering||sun||sky_position||
          (baseline&&orientation),
      .sky_irradiance=optical||scattering||sun||sky_position,
      .aerial_perspective=
          optical||scattering||sun||position||orientation||shadow||origin,
      .long_shadow=!baseline&&
          (optical||scattering||sun||position||orientation||shadow||origin),
  };
}

std::uint64_t atmosphere_optical_hash(
    const AtmosphereParameters& parameters) {
  std::uint64_t hash=1469598103934665603ULL;
  hash_double(hash,parameters.ground_radius_metres);
  hash_double(hash,parameters.atmosphere_height_metres);
  for(double value:parameters.rayleigh_scattering_per_metre)
    hash_double(hash,value);
  hash_double(hash,parameters.rayleigh_scale_height_metres);
  for(double value:parameters.mie_scattering_per_metre)
    hash_double(hash,value);
  for(double value:parameters.mie_absorption_per_metre)
    hash_double(hash,value);
  hash_double(hash,parameters.mie_scale_height_metres);
  for(double value:parameters.absorption_per_metre)hash_double(hash,value);
  hash_double(hash,parameters.absorption_peak_altitude_metres);
  hash_double(hash,parameters.absorption_half_width_metres);
  return hash;
}

std::uint64_t atmosphere_scattering_hash(
    const AtmosphereParameters& parameters) {
  std::uint64_t hash=atmosphere_optical_hash(parameters);
  hash_double(hash,parameters.mie_anisotropy);
  for(double value:parameters.ground_albedo)hash_double(hash,value);
  for(double value:parameters.solar_irradiance)hash_double(hash,value);
  hash_double(hash,parameters.solar_angular_radius_radians);
  return hash;
}

AtmosphereSkyPositionRevision atmosphere_sky_position_revision(
    tetra::Vec3 position, const AtmosphereParameters& parameters) noexcept {
  const double radial_distance=length(position);
  if(!(radial_distance>0.0)||!std::isfinite(radial_distance))return {};
  const tetra::Vec3 up=position/radial_distance;
  // Full-sky radiance changes materially with altitude through the narrowest
  // density profile, while horizontal motion changes only its local frame.
  // Quantize below both effects' lookup resolution so centimetre-scale player
  // motion cannot regenerate a planet-scale lighting table every frame.
  const double altitude_step=std::max(0.25,
      std::min(parameters.rayleigh_scale_height_metres,
               parameters.mie_scale_height_metres)/64.0);
  constexpr double up_step=1.0e-4;
  const auto quantize=[](double value,double step){
    return std::nearbyint(value/step)*step;
  };
  std::uint64_t hash=1469598103934665603ULL;
  hash_double(hash,quantize(radial_distance-parameters.ground_radius_metres,
                            altitude_step));
  hash_double(hash,quantize(up.x,up_step));
  hash_double(hash,quantize(up.y,up_step));
  hash_double(hash,quantize(up.z,up_step));
  return {hash};
}

AtmosphereMaterialSnapshot atmosphere_material_snapshot(
    const AtmosphereParameters& parameters,AtmosphereTransport transport) {
  return {.parameters=parameters,
          .optical={atmosphere_optical_hash(parameters)},
          .scattering={atmosphere_scattering_hash(parameters)},
          .transport=transport};
}

AtmosphereLookupSnapshotSet advance_atmosphere_lookup_snapshots(
    std::optional<AtmosphereLookupSnapshotSet> previous,
    const AtmosphereMaterialSnapshot& material,
    const AtmosphereLookupRevisions& next,
    const AtmosphereDispatchPlan& dispatch) {
  AtmosphereLookupSnapshotSet result=previous.value_or(
      AtmosphereLookupSnapshotSet{});
  if(dispatch.transmittance)
    result.optical=AtmosphereOpticalLookupSnapshot{
        next.optical,material.transport};
  if(dispatch.multiple_scattering||dispatch.sky_view||dispatch.sky_irradiance)
    result.lighting=AtmosphereLightingLookupSnapshot{
        next.optical,next.scattering,next.sun,next.sky_position,
        next.camera_orientation,material.transport};
  if(dispatch.aerial_perspective||dispatch.long_shadow)
    result.view=AtmosphereViewLookupSnapshot{
        next.optical,next.scattering,next.sun,next.camera_position,
        next.camera_orientation,next.shadow,next.render_origin,
        material.transport};
  return result;
}

AtmosphereValidationSnapshot atmosphere_validation_snapshot(
    const AtmosphereMaterialSnapshot& material,
    const AtmosphereLookupSnapshotSet& lookups,
    const AtmosphereLookupRevisions& frame) {
  AtmosphereValidationSnapshot result{material,lookups,frame,std::nullopt};
  const auto reject=[&](std::string message){
    if(!result.incompatibility)result.incompatibility=std::move(message);
  };
  if(const auto invalid=validate_atmosphere(material.parameters))reject(*invalid);
  if(material.optical.value!=atmosphere_optical_hash(material.parameters))
    reject("material optical revision does not match its parameters");
  if(material.scattering.value!=
     atmosphere_scattering_hash(material.parameters))
    reject("material scattering revision does not match its parameters");
  if(material.optical!=frame.optical||material.scattering!=frame.scattering)
    reject("frame material revisions are incompatible");
  if(!lookups.optical)reject("optical lookup snapshot is missing");
  else if(lookups.optical->transport!=material.transport||
          lookups.optical->optical!=frame.optical)
    reject("optical lookup generation is incompatible");
  if(!lookups.lighting)reject("lighting lookup snapshot is missing");
  else if(lookups.lighting->transport!=material.transport||
          lookups.lighting->optical!=frame.optical||
          lookups.lighting->scattering!=frame.scattering||
          lookups.lighting->sun!=frame.sun||
          lookups.lighting->sky_position!=frame.sky_position||
          (material.transport==AtmosphereTransport::qualified_baseline&&
           lookups.lighting->camera_orientation!=frame.camera_orientation))
    reject("lighting lookup generation is incompatible");
  if(!lookups.view)reject("view lookup snapshot is missing");
  else if(lookups.view->transport!=material.transport||
          lookups.view->optical!=frame.optical||
          lookups.view->scattering!=frame.scattering||
          lookups.view->sun!=frame.sun||
          lookups.view->camera_position!=frame.camera_position||
          lookups.view->camera_orientation!=frame.camera_orientation||
          lookups.view->shadow!=frame.shadow||
          lookups.view->render_origin!=frame.render_origin)
    reject("view lookup generation is incompatible");
  return result;
}

AtmosphereParameters atmosphere_preset(AtmospherePreset preset) {
  AtmosphereParameters result;
  switch (preset) {
    case AtmospherePreset::gameplay_planet:
      // A deliberately compact world with Earth-like vertical optical depth.
      // Shorter density profiles are paired with proportionally stronger
      // coefficients; this keeps the light transport coherent without
      // pretending that a 200 km natural body could retain Earth's air.
      result.ground_radius_metres = 200'000.0;
      result.atmosphere_height_metres = 20'000.0;
      result.rayleigh_scale_height_metres = 3'000.0;
      result.rayleigh_scattering_per_metre = {
          15.472e-6, 36.1546666666667e-6, 88.2666666666667e-6};
      // Preserve Earth's integrated aerosol optical depth, but concentrate it
      // into a shallow gameplay-scale boundary layer. On a 200 km planet the
      // eye-level geometric horizon is only a few kilometres away; scaling
      // the aerosol layer with the whole atmosphere made that entire useful
      // range look crystal clear.
      result.mie_scale_height_metres = 30.0;
      result.mie_scattering_per_metre = {
          159.84e-6, 159.84e-6, 159.84e-6};
      result.mie_absorption_per_metre = {
          16.16e-6, 16.16e-6, 16.16e-6};
      result.absorption_peak_altitude_metres = 8'000.0;
      result.absorption_half_width_metres = 4'500.0;
      result.absorption_per_metre = {
          2.16666666666667e-6, 6.27e-6, 0.283333333333333e-6};
      // The analytic continuation uses the same neutral stone reflectance as
      // the generated terrain, avoiding a dark lower-hemisphere seam where
      // the finite active mesh hands off to the planet surface.
      result.ground_albedo = {0.32, 0.33, 0.34};
      return result;
    case AtmospherePreset::earth:
    case AtmospherePreset::custom:
      return result;
    case AtmospherePreset::mars_like:
      result.ground_radius_metres = 3'389'500.0;
      result.atmosphere_height_metres = 80'000.0;
      result.rayleigh_scattering_per_metre = {1.0e-7, 2.0e-7, 4.0e-7};
      result.rayleigh_scale_height_metres = 11'100.0;
      result.mie_scattering_per_metre = {15.0e-6, 9.0e-6, 3.0e-6};
      result.mie_absorption_per_metre = {2.0e-6, 2.5e-6, 3.0e-6};
      result.mie_scale_height_metres = 10'000.0;
      result.mie_anisotropy = 0.72;
      result.absorption_per_metre = {0.0, 0.0, 0.0};
      result.ground_albedo = {0.20, 0.08, 0.035};
      result.solar_irradiance = {0.43, 0.43, 0.42};
      result.solar_angular_radius_radians = 0.0031;
      return result;
    case AtmospherePreset::dense_haze:
      result.ground_radius_metres = 6'050'000.0;
      result.atmosphere_height_metres = 180'000.0;
      result.rayleigh_scattering_per_metre = {9.0e-6, 18.0e-6, 30.0e-6};
      result.rayleigh_scale_height_metres = 15'000.0;
      result.mie_scattering_per_metre = {25.0e-6, 20.0e-6, 12.0e-6};
      result.mie_absorption_per_metre = {8.0e-6, 10.0e-6, 14.0e-6};
      result.mie_scale_height_metres = 8'000.0;
      result.mie_anisotropy = 0.65;
      result.absorption_per_metre = {2.0e-6, 5.0e-6, 8.0e-6};
      result.absorption_peak_altitude_metres = 45'000.0;
      result.absorption_half_width_metres = 30'000.0;
      result.ground_albedo = {0.16, 0.14, 0.10};
      result.solar_irradiance = {0.75, 0.68, 0.55};
      return result;
    case AtmospherePreset::nearly_airless:
      result.ground_radius_metres = 1'737'400.0;
      result.atmosphere_height_metres = 20'000.0;
      result.rayleigh_scattering_per_metre = {1.0e-10, 2.0e-10, 4.0e-10};
      result.rayleigh_scale_height_metres = 4'000.0;
      result.mie_scattering_per_metre = {1.0e-10, 1.0e-10, 1.0e-10};
      result.mie_absorption_per_metre = {0.0, 0.0, 0.0};
      result.mie_scale_height_metres = 1'000.0;
      result.absorption_per_metre = {0.0, 0.0, 0.0};
      result.ground_albedo = {0.12, 0.12, 0.12};
      return result;
  }
  return result;
}

std::optional<AtmospherePreset> parse_atmosphere_preset(std::string_view name) {
  if (name == "gameplay-planet") return AtmospherePreset::gameplay_planet;
  if (name == "earth") return AtmospherePreset::earth;
  if (name == "mars-like") return AtmospherePreset::mars_like;
  if (name == "dense-haze") return AtmospherePreset::dense_haze;
  if (name == "nearly-airless") return AtmospherePreset::nearly_airless;
  if (name == "custom") return AtmospherePreset::custom;
  return std::nullopt;
}

std::string_view atmosphere_preset_name(AtmospherePreset preset) {
  switch (preset) {
    case AtmospherePreset::gameplay_planet: return "gameplay-planet";
    case AtmospherePreset::earth: return "earth";
    case AtmospherePreset::mars_like: return "mars-like";
    case AtmospherePreset::dense_haze: return "dense-haze";
    case AtmospherePreset::nearly_airless: return "nearly-airless";
    case AtmospherePreset::custom: return "custom";
  }
  return "custom";
}

std::optional<std::string> validate_atmosphere(
    const AtmosphereParameters& parameters) {
  const auto positive_finite = [](double value) {
    return value > 0.0 && std::isfinite(value);
  };
  if (!positive_finite(parameters.ground_radius_metres))
    return "ground radius must be positive and finite";
  if (!positive_finite(parameters.atmosphere_height_metres))
    return "atmosphere height must be positive and finite";
  if (!std::isfinite(parameters.ground_radius_metres +
                     parameters.atmosphere_height_metres))
    return "atmosphere top radius must be finite";
  if (!positive_finite(parameters.metres_per_world_unit))
    return "metres per world unit must be positive and finite";
  if (!positive_finite(parameters.rayleigh_scale_height_metres) ||
      !positive_finite(parameters.mie_scale_height_metres))
    return "density scale heights must be positive and finite";
  if (!std::isfinite(parameters.mie_anisotropy) ||
      std::abs(parameters.mie_anisotropy) >= 1.0)
    return "Mie anisotropy must be finite and have magnitude below one";
  if (!std::isfinite(parameters.absorption_peak_altitude_metres) ||
      parameters.absorption_peak_altitude_metres < 0.0 ||
      !positive_finite(parameters.absorption_half_width_metres))
    return "absorption profile must be finite and nonnegative";
  if (!positive_finite(parameters.solar_angular_radius_radians) ||
      parameters.solar_angular_radius_radians >= std::numbers::pi / 2.0)
    return "solar angular radius is invalid";
  const std::array spectra{
      parameters.rayleigh_scattering_per_metre,
      parameters.mie_scattering_per_metre,
      parameters.mie_absorption_per_metre,
      parameters.absorption_per_metre,
      parameters.ground_albedo,
      parameters.solar_irradiance};
  for (const auto& spectrum : spectra)
    if (!finite_spectrum(spectrum) || !nonnegative_spectrum(spectrum))
      return "spectral parameters must be finite and nonnegative";
  for (double albedo : parameters.ground_albedo)
    if (albedo > 1.0) return "ground albedo must not exceed one";
  return std::nullopt;
}

std::uint64_t atmosphere_parameter_hash(
    const AtmosphereParameters& parameters) {
  std::uint64_t hash = 1469598103934665603ULL;
  hash_double(hash, parameters.ground_radius_metres);
  hash_double(hash, parameters.atmosphere_height_metres);
  hash_double(hash, parameters.metres_per_world_unit);
  for (double value : parameters.rayleigh_scattering_per_metre)
    hash_double(hash, value);
  hash_double(hash, parameters.rayleigh_scale_height_metres);
  for (double value : parameters.mie_scattering_per_metre) hash_double(hash, value);
  for (double value : parameters.mie_absorption_per_metre) hash_double(hash, value);
  hash_double(hash, parameters.mie_scale_height_metres);
  hash_double(hash, parameters.mie_anisotropy);
  for (double value : parameters.absorption_per_metre) hash_double(hash, value);
  hash_double(hash, parameters.absorption_peak_altitude_metres);
  hash_double(hash, parameters.absorption_half_width_metres);
  for (double value : parameters.ground_albedo) hash_double(hash, value);
  for (double value : parameters.solar_irradiance) hash_double(hash, value);
  hash_double(hash, parameters.solar_angular_radius_radians);
  return hash;
}

std::string serialize_atmosphere_parameters(
    const AtmosphereParameters& parameters) {
  std::ostringstream output;
  output << std::setprecision(17)
         << "{\"ground_radius_metres\":" << parameters.ground_radius_metres
         << ",\"atmosphere_height_metres\":"
         << parameters.atmosphere_height_metres
         << ",\"metres_per_world_unit\":" << parameters.metres_per_world_unit
         << ",\"rayleigh\":[" << parameters.rayleigh_scattering_per_metre[0]
         << ',' << parameters.rayleigh_scattering_per_metre[1] << ','
         << parameters.rayleigh_scattering_per_metre[2]
         << "],\"rayleigh_scale_height_metres\":"
         << parameters.rayleigh_scale_height_metres << ",\"mie_scattering\":["
         << parameters.mie_scattering_per_metre[0] << ','
         << parameters.mie_scattering_per_metre[1] << ','
         << parameters.mie_scattering_per_metre[2]
         << "],\"mie_absorption\":[" << parameters.mie_absorption_per_metre[0]
         << ',' << parameters.mie_absorption_per_metre[1] << ','
         << parameters.mie_absorption_per_metre[2]
         << "],\"mie_scale_height_metres\":"
         << parameters.mie_scale_height_metres << ",\"mie_anisotropy\":"
         << parameters.mie_anisotropy << ",\"absorption\":["
         << parameters.absorption_per_metre[0] << ','
         << parameters.absorption_per_metre[1] << ','
         << parameters.absorption_per_metre[2]
         << "],\"absorption_peak_altitude_metres\":"
         << parameters.absorption_peak_altitude_metres
         << ",\"absorption_half_width_metres\":"
         << parameters.absorption_half_width_metres << ",\"ground_albedo\":["
         << parameters.ground_albedo[0] << ',' << parameters.ground_albedo[1]
         << ',' << parameters.ground_albedo[2] << "],\"solar_irradiance\":["
         << parameters.solar_irradiance[0] << ','
         << parameters.solar_irradiance[1] << ','
         << parameters.solar_irradiance[2]
         << "],\"solar_angular_radius_radians\":"
         << parameters.solar_angular_radius_radians << '}';
  return output.str();
}

AtmosphereInvalidation atmosphere_invalidation(
    const AtmosphereParameters& before, const AtmosphereParameters& after) {
  const bool optical = extinction_changed(before, after);
  const bool albedo = !equal(before.ground_albedo, after.ground_albedo);
  const bool phase = before.mie_anisotropy != after.mie_anisotropy;
  const bool solar = !equal(before.solar_irradiance, after.solar_irradiance) ||
                     before.solar_angular_radius_radians !=
                         after.solar_angular_radius_radians;
  return {optical, optical || albedo || phase || solar,
          optical || albedo || phase || solar,
          optical || albedo || phase || solar};
}

std::optional<AtmosphereRaySegment> atmosphere_ray_segment(
    tetra::Vec3 position, tetra::Vec3 direction,
    const AtmosphereParameters& parameters) {
  if (validate_atmosphere(parameters)) return std::nullopt;
  direction = normalized(direction);
  if (length(direction) == 0.0) return std::nullopt;
  const double top_radius = parameters.ground_radius_metres +
                            parameters.atmosphere_height_metres;
  const auto outer = sphere_roots(position, direction, top_radius);
  if (!outer || (*outer)[1] < 0.0) return std::nullopt;
  double begin = std::max(0.0, (*outer)[0]);
  double end = (*outer)[1];
  const double radial_distance=length(position);
  const double ground_radius=parameters.ground_radius_metres;
  if(radial_distance<ground_radius){
    if(const auto ground=sphere_roots(position,direction,ground_radius))
      begin=std::max(begin,(*ground)[1]);
  }else{
    const double altitude=radial_distance-ground_radius;
    const double radial_cosine=dot(position/radial_distance,direction);
    const double horizon_sine_squared=std::max(0.0,
        altitude*(radial_distance+ground_radius)/
            (radial_distance*radial_distance));
    const double cosine_squared=radial_cosine*radial_cosine;
    // A tangent touches the boundary at one zero-measure point but never
    // enters the opaque sphere, so only a strict crossing terminates medium.
    if(radial_cosine<0.0&&cosine_squared>
       horizon_sine_squared*(1.0+2.0e-12)){
      const double root=radial_distance*(-radial_cosine-
          std::sqrt(std::max(0.0,cosine_squared-horizon_sine_squared)));
      if(root>begin+1.0e-7)end=std::min(end,root);
    }
  }
  if (!(end > begin)) return std::nullopt;
  return AtmosphereRaySegment{begin, end};
}

double atmosphere_rayleigh_density(double altitude,
                                    const AtmosphereParameters& parameters) {
  if (altitude < 0.0 || altitude > parameters.atmosphere_height_metres)
    return 0.0;
  return std::exp(-altitude / parameters.rayleigh_scale_height_metres);
}

double atmosphere_mie_density(double altitude,
                              const AtmosphereParameters& parameters) {
  if (altitude < 0.0 || altitude > parameters.atmosphere_height_metres)
    return 0.0;
  return std::exp(-altitude / parameters.mie_scale_height_metres);
}

double atmosphere_absorption_density(
    double altitude, const AtmosphereParameters& parameters) {
  if (altitude < 0.0 || altitude > parameters.atmosphere_height_metres)
    return 0.0;
  return std::max(0.0, 1.0 -
      std::abs(altitude - parameters.absorption_peak_altitude_metres) /
          parameters.absorption_half_width_metres);
}

double rayleigh_phase(double cosine_angle) {
  cosine_angle = std::clamp(cosine_angle, -1.0, 1.0);
  return 3.0 * (1.0 + cosine_angle * cosine_angle) /
         (16.0 * std::numbers::pi);
}

double mie_henyey_greenstein_phase(double cosine_angle, double anisotropy) {
  cosine_angle = std::clamp(cosine_angle, -1.0, 1.0);
  anisotropy = std::clamp(anisotropy, -0.999999, 0.999999);
  const double denominator = std::pow(
      1.0 + anisotropy * anisotropy -
          2.0 * anisotropy * cosine_angle,
      1.5);
  return (1.0 - anisotropy * anisotropy) /
         (4.0 * std::numbers::pi * denominator);
}

AtmosphereOpticalDepth atmosphere_optical_depth(
    tetra::Vec3 start, tetra::Vec3 end,
    const AtmosphereParameters& parameters, std::size_t steps) {
  if (steps < 2U) steps = 2U;
  if ((steps & 1U) != 0U) ++steps;
  const tetra::Vec3 delta = end - start;
  const double distance = length(delta);
  if (!(distance > 0.0) || validate_atmosphere(parameters)) return {};
  AtmosphereOpticalDepth sum;
  for (std::size_t index = 0; index <= steps; ++index) {
    const double t = static_cast<double>(index) / static_cast<double>(steps);
    const tetra::Vec3 point = start + delta * t;
    const double altitude = length(point) - parameters.ground_radius_metres;
    const double weight = index == 0U || index == steps ? 1.0 :
                          ((index & 1U) != 0U ? 4.0 : 2.0);
    sum.rayleigh += weight * atmosphere_rayleigh_density(altitude, parameters);
    sum.mie += weight * atmosphere_mie_density(altitude, parameters);
    sum.absorption +=
        weight * atmosphere_absorption_density(altitude, parameters);
  }
  const double scale = distance / (3.0 * static_cast<double>(steps));
  sum.rayleigh *= scale;
  sum.mie *= scale;
  sum.absorption *= scale;
  return sum;
}

AtmosphereSpectrum atmosphere_transmittance(
    tetra::Vec3 start, tetra::Vec3 end,
    const AtmosphereParameters& parameters, std::size_t steps) {
  const auto depth = atmosphere_optical_depth(start, end, parameters, steps);
  AtmosphereSpectrum result{};
  for (std::size_t channel = 0; channel < result.size(); ++channel) {
    const double extinction =
        parameters.rayleigh_scattering_per_metre[channel] * depth.rayleigh +
        (parameters.mie_scattering_per_metre[channel] +
         parameters.mie_absorption_per_metre[channel]) * depth.mie +
        parameters.absorption_per_metre[channel] * depth.absorption;
    result[channel] = std::exp(-std::max(0.0, extinction));
  }
  return result;
}

AtmosphereLookupCoordinates atmosphere_transmittance_uv(
    double altitude, double zenith_cosine,
    const AtmosphereParameters& parameters) noexcept {
  const double ground=parameters.ground_radius_metres;
  const double top=ground+parameters.atmosphere_height_metres;
  if(!(ground>0.0)||!(top>ground)||!std::isfinite(altitude)||
     !std::isfinite(zenith_cosine))return {};
  altitude=std::clamp(altitude,0.0,parameters.atmosphere_height_metres);
  zenith_cosine=std::clamp(zenith_cosine,-1.0,1.0);
  const double radius=ground+altitude;
  const double horizon=std::sqrt(
      std::max(0.0,(top-ground)*(top+ground)));
  const double rho=std::sqrt(
      std::max(0.0,(radius-ground)*(radius+ground)));
  const double discriminant=std::max(0.0,
      radius*radius*(zenith_cosine*zenith_cosine-1.0)+top*top);
  const double distance=std::max(
      0.0,-radius*zenith_cosine+std::sqrt(discriminant));
  const double minimum=top-radius;
  const double maximum=rho+horizon;
  const double range=std::max(maximum-minimum,
                              std::numeric_limits<double>::min());
  return {std::clamp((distance-minimum)/range,0.0,1.0),
          std::clamp(rho/horizon,0.0,1.0)};
}

AtmosphereTransmittanceParameters atmosphere_transmittance_parameters(
    AtmosphereLookupCoordinates uv,
    const AtmosphereParameters& parameters) noexcept {
  const double ground=parameters.ground_radius_metres;
  const double top=ground+parameters.atmosphere_height_metres;
  if(!(ground>0.0)||!(top>ground)||!std::isfinite(uv.u)||
     !std::isfinite(uv.v))return {};
  uv.u=std::clamp(uv.u,0.0,1.0);
  uv.v=std::clamp(uv.v,0.0,1.0);
  const double horizon=std::sqrt(
      std::max(0.0,(top-ground)*(top+ground)));
  const double rho=horizon*uv.v;
  const double radial_distance=std::sqrt(rho*rho+ground*ground);
  const double altitude=rho*rho/(radial_distance+ground);
  const double radius=ground+altitude;
  const double minimum=top-radius;
  const double maximum=rho+horizon;
  const double distance=minimum+uv.u*(maximum-minimum);
  double cosine=1.0;
  if(uv.u>=1.0-1.0e-14)cosine=-rho/radius;
  else if(distance>1.0e-12){
    const double top_minus_radius_squared=(top-radius)*(top+radius);
    cosine=(top_minus_radius_squared-distance*distance)/
        (2.0*radius*distance);
  }
  return {std::clamp(altitude,0.0,
                     parameters.atmosphere_height_metres),
          std::clamp(cosine,-1.0,1.0)};
}

AtmosphereLookupCoordinates atmosphere_full_sky_uv(
    tetra::Vec3 direction,tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept {
  const auto basis=sky_basis(local_up,sun_direction);
  direction=normalized(direction);
  if(length(direction)==0.0)return {0.5,0.5};
  const double tangent_x=dot(direction,basis.sun_tangent);
  const double tangent_y=dot(direction,basis.longitude_tangent);
  double perimeter=0.0;
  if(tangent_x>=0.0){
    const double denominator=std::abs(tangent_x)+std::abs(tangent_y);
    perimeter=denominator>0.0?tangent_y/denominator:0.0;
  }else if(tangent_y>=0.0){
    const double denominator=-tangent_x+tangent_y;
    perimeter=2.0-tangent_y/denominator;
  }else{
    const double denominator=-tangent_x-tangent_y;
    perimeter=-1.0+tangent_x/denominator;
  }
  const double vertical=std::clamp(dot(direction,basis.up),-1.0,1.0);
  constexpr double latitude_shape=std::numbers::pi/4.0-1.0;
  const double root=std::sqrt(std::max(0.0,1.0-std::abs(vertical)));
  const double latitude_proxy=(1.0-root)/(1.0+latitude_shape*root);
  const double mapped_latitude=std::abs(vertical)<1.0e-14?0.0:
      std::copysign(std::sqrt(latitude_proxy),vertical);
  return {perimeter*0.25+0.5,
          mapped_latitude*0.5+0.5};
}

tetra::Vec3 atmosphere_full_sky_direction(
    AtmosphereLookupCoordinates uv,tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept {
  const auto basis=sky_basis(local_up,sun_direction);
  uv.u=std::clamp(std::isfinite(uv.u)?uv.u:0.5,0.0,1.0);
  uv.v=std::clamp(std::isfinite(uv.v)?uv.v:0.5,0.0,1.0);
  const double perimeter=(uv.u-0.5)*4.0;
  tetra::Vec3 tangent;
  if(perimeter>=0.0&&perimeter<=1.0)
    tangent=basis.sun_tangent*(1.0-perimeter)+
        basis.longitude_tangent*perimeter;
  else if(perimeter>1.0){
    const double offset=perimeter-1.0;
    tangent=basis.sun_tangent*(-offset)+
        basis.longitude_tangent*(1.0-offset);
  }else if(perimeter>=-1.0)
    tangent=basis.sun_tangent*(1.0+perimeter)+
        basis.longitude_tangent*perimeter;
  else{
    const double offset=-perimeter-1.0;
    tangent=basis.sun_tangent*(-offset)+
        basis.longitude_tangent*(-1.0+offset);
  }
  tangent=normalized(tangent);
  const double mapped_latitude=uv.v*2.0-1.0;
  constexpr double latitude_shape=std::numbers::pi/4.0-1.0;
  const double latitude_proxy=mapped_latitude*mapped_latitude;
  const double root=(1.0-latitude_proxy)/
      (1.0+latitude_shape*latitude_proxy);
  const double vertical=std::copysign(1.0-root*root,mapped_latitude);
  const double horizontal=std::sqrt(std::max(0.0,1.0-vertical*vertical));
  return normalized(tangent*horizontal+basis.up*vertical);
}

AtmosphereSpectrum atmosphere_multiple_scattering_closure(
    const AtmosphereSpectrum& second_order,
    const AtmosphereSpectrum& transfer_factor) noexcept {
  AtmosphereSpectrum result{};
  for(std::size_t channel=0;channel<result.size();++channel){
    const double radiance=std::isfinite(second_order[channel])?
        std::max(0.0,second_order[channel]):0.0;
    const double transfer=std::isfinite(transfer_factor[channel])?
        std::clamp(transfer_factor[channel],0.0,0.999):0.0;
    result[channel]=radiance/(1.0-transfer);
  }
  return result;
}

AtmosphereMultipleScatteringReference
atmosphere_multiple_scattering_reference(
    const AtmosphereParameters& parameters, double altitude,
    double sun_zenith_cosine, std::size_t direction_count,
    std::size_t ray_steps) {
  AtmosphereMultipleScatteringReference result;
  if(validate_atmosphere(parameters)||!std::isfinite(altitude)||
     !std::isfinite(sun_zenith_cosine))return result;
  altitude=std::clamp(altitude,0.0,parameters.atmosphere_height_metres);
  sun_zenith_cosine=std::clamp(sun_zenith_cosine,-1.0,1.0);
  direction_count=std::clamp<std::size_t>(direction_count,1U,4096U);
  ray_steps=std::clamp<std::size_t>(ray_steps,1U,4096U);
  const tetra::Vec3 origin{0.0,parameters.ground_radius_metres+altitude,0.0};
  const tetra::Vec3 sun_direction{
      std::sqrt(std::max(0.0,1.0-sun_zenith_cosine*sun_zenith_cosine)),
      sun_zenith_cosine,0.0};
  const AtmosphereSpectrum zero{};
  const auto sun_transmittance=[&](tetra::Vec3 point){
    if(const auto ground=sphere_roots(point,sun_direction,
                                      parameters.ground_radius_metres))
      for(const double root:*ground)if(root>1.0e-5)return zero;
    const auto segment=atmosphere_ray_segment(point,sun_direction,parameters);
    if(!segment)return AtmosphereSpectrum{1.0,1.0,1.0};
    return atmosphere_transmittance(
        point+sun_direction*segment->begin_metres,
        point+sun_direction*segment->end_metres,parameters,64U);
  };

  for(std::size_t direction_index=0;direction_index<direction_count;
      ++direction_index){
    const double y=1.0-2.0*(static_cast<double>(direction_index)+0.5)/
        static_cast<double>(direction_count);
    const double angle=static_cast<double>(direction_index)*2.399963229728653;
    const double radial=std::sqrt(std::max(0.0,1.0-y*y));
    const tetra::Vec3 direction{radial*std::cos(angle),y,
                                radial*std::sin(angle)};
    const auto segment=atmosphere_ray_segment(origin,direction,parameters);
    if(!segment)continue;
    const double step_length=(segment->end_metres-segment->begin_metres)/
        static_cast<double>(ray_steps);
    AtmosphereSpectrum view_transmittance{1.0,1.0,1.0};
    for(std::size_t step=0;step<ray_steps;++step){
      const double distance=segment->begin_metres+
          (static_cast<double>(step)+0.5)*step_length;
      const tetra::Vec3 point=origin+direction*distance;
      const double local_altitude=length(point)-
          parameters.ground_radius_metres;
      const double rayleigh=atmosphere_rayleigh_density(
          local_altitude,parameters);
      const double mie=atmosphere_mie_density(local_altitude,parameters);
      const double absorption=atmosphere_absorption_density(
          local_altitude,parameters);
      const auto solar=sun_transmittance(point);
      for(std::size_t channel=0;channel<3U;++channel){
        const double scattering=
            parameters.rayleigh_scattering_per_metre[channel]*rayleigh+
            parameters.mie_scattering_per_metre[channel]*mie;
        const double extinction=scattering+
            parameters.mie_absorption_per_metre[channel]*mie+
            parameters.absorption_per_metre[channel]*absorption;
        const double segment_transmittance=
            std::exp(-std::max(0.0,extinction)*step_length);
        const double integral=extinction>1.0e-15?
            (1.0-segment_transmittance)/extinction:step_length;
        result.second_order[channel]+=view_transmittance[channel]*
            scattering*solar[channel]*integral/(4.0*std::numbers::pi);
        result.transfer_factor[channel]+=view_transmittance[channel]*
            scattering*integral;
        view_transmittance[channel]*=segment_transmittance;
      }
    }

    const tetra::Vec3 endpoint=origin+direction*segment->end_metres;
    if(length(endpoint)<=parameters.ground_radius_metres+5.0){
      const tetra::Vec3 normal=normalized(endpoint);
      const auto solar=sun_transmittance(endpoint+normal*1.0e-3);
      const double n_dot_l=std::max(0.0,dot(normal,sun_direction));
      for(std::size_t channel=0;channel<3U;++channel)
        result.second_order[channel]+=view_transmittance[channel]*
            parameters.ground_albedo[channel]*solar[channel]*n_dot_l/
            std::numbers::pi;
    }
  }
  for(std::size_t channel=0;channel<3U;++channel){
    result.second_order[channel]/=static_cast<double>(direction_count);
    result.transfer_factor[channel]/=static_cast<double>(direction_count);
  }
  result.closed_contribution=atmosphere_multiple_scattering_closure(
      result.second_order,result.transfer_factor);
  return result;
}

AtmosphereSpectrum atmosphere_sky_irradiance_reference(
    tetra::Vec3 surface_normal, tetra::Vec3 local_up,
    const AtmosphereSkyRadianceFunction& sky_radiance,
    std::size_t direction_count) {
  AtmosphereSpectrum result{};
  surface_normal=normalized(surface_normal);
  local_up=normalized(local_up);
  if(length(surface_normal)==0.0||length(local_up)==0.0||!sky_radiance||
     direction_count==0U)return result;

  constexpr double golden_angle=2.3999632297286533222;
  for(std::size_t index=0;index<direction_count;++index){
    const double y=1.0-2.0*(static_cast<double>(index)+0.5)/
        static_cast<double>(direction_count);
    const double radial=std::sqrt(std::max(0.0,1.0-y*y));
    const double angle=static_cast<double>(index)*golden_angle;
    const tetra::Vec3 direction{radial*std::cos(angle),y,
                                radial*std::sin(angle)};
    const double weight=std::max(0.0,dot(surface_normal,direction));
    if(weight==0.0||dot(local_up,direction)<=0.0)continue;
    const auto radiance=sky_radiance(direction);
    for(std::size_t channel=0;channel<result.size();++channel)
      if(std::isfinite(radiance[channel])&&radiance[channel]>0.0)
        result[channel]+=radiance[channel]*weight;
  }
  const double normalization=4.0/static_cast<double>(direction_count);
  for(auto& channel:result)channel*=normalization;
  return result;
}

AtmosphereScatteringReference atmosphere_scattering_reference(
    const AtmosphereParameters& parameters, tetra::Vec3 origin,
    tetra::Vec3 view_direction, tetra::Vec3 sun_direction,
    double maximum_distance, std::size_t view_steps,
    std::size_t multiple_direction_count,
    std::size_t multiple_ray_steps) {
  AtmosphereScatteringReference result;
  if(validate_atmosphere(parameters)||!(maximum_distance>0.0)||
     !std::isfinite(maximum_distance))return result;
  view_direction=normalized(view_direction);
  sun_direction=normalized(sun_direction);
  if(length(view_direction)==0.0||length(sun_direction)==0.0)return result;
  const auto ray=atmosphere_ray_segment(origin,view_direction,parameters);
  if(!ray||maximum_distance<=ray->begin_metres)return result;
  const double begin=ray->begin_metres;
  const double end=std::min(ray->end_metres,maximum_distance);
  if(!(end>begin))return result;
  view_steps=std::clamp<std::size_t>(view_steps,1U,4096U);
  multiple_direction_count=std::clamp<std::size_t>(
      multiple_direction_count,1U,256U);
  multiple_ray_steps=std::clamp<std::size_t>(multiple_ray_steps,1U,256U);
  const double phase_cosine=dot(view_direction,sun_direction);
  const double rayleigh_phase_value=rayleigh_phase(phase_cosine);
  const double mie_phase_value=mie_henyey_greenstein_phase(
      phase_cosine,parameters.mie_anisotropy);
  AtmosphereSpectrum path_transmittance{1.0,1.0,1.0};
  const double segment_length=end-begin;
  const AtmosphereSpectrum zero{};
  const double view_step_count=static_cast<double>(view_steps);
  const auto sunlight=[&](tetra::Vec3 point){
    if(const auto ground=sphere_roots(point,sun_direction,
                                      parameters.ground_radius_metres))
      for(const double root:*ground)if(root>1.0e-5)return zero;
    const auto segment=atmosphere_ray_segment(point,sun_direction,parameters);
    if(!segment)return AtmosphereSpectrum{1.0,1.0,1.0};
    return atmosphere_transmittance(
        point+sun_direction*segment->begin_metres,
        point+sun_direction*segment->end_metres,parameters,128U);
  };
  for(std::size_t step=0;step<view_steps;++step){
    const double fraction_begin=static_cast<double>(step)/view_step_count;
    const double fraction_end=static_cast<double>(step+1U)/view_step_count;
    const double distance_begin=begin+
        segment_length*fraction_begin*fraction_begin;
    const double distance_end=begin+
        segment_length*fraction_end*fraction_end;
    const double step_length=distance_end-distance_begin;
    const tetra::Vec3 point=origin+view_direction*
        (0.5*(distance_begin+distance_end));
    const double altitude=length(point)-parameters.ground_radius_metres;
    const double rayleigh=atmosphere_rayleigh_density(altitude,parameters);
    const double mie=atmosphere_mie_density(altitude,parameters);
    const double absorption=atmosphere_absorption_density(altitude,parameters);
    const auto solar_transmittance=sunlight(point);
    const double sun_cosine=dot(normalized(point),sun_direction);
    const auto multiple=atmosphere_multiple_scattering_reference(
        parameters,altitude,sun_cosine,multiple_direction_count,
        multiple_ray_steps).closed_contribution;
    for(std::size_t channel=0;channel<3U;++channel){
      const double rayleigh_scattering=
          parameters.rayleigh_scattering_per_metre[channel]*rayleigh;
      const double mie_scattering=
          parameters.mie_scattering_per_metre[channel]*mie;
      const double extinction=rayleigh_scattering+mie_scattering+
          parameters.mie_absorption_per_metre[channel]*mie+
          parameters.absorption_per_metre[channel]*absorption;
      const double segment_transmittance=
          std::exp(-std::max(0.0,extinction)*step_length);
      const double integral=extinction>1.0e-15?
          (1.0-segment_transmittance)/extinction:step_length;
      const double direct=(rayleigh_scattering*rayleigh_phase_value+
          mie_scattering*mie_phase_value)*solar_transmittance[channel]*
          parameters.solar_irradiance[channel];
      const double higher_order=(rayleigh_scattering+mie_scattering)*
          multiple[channel]*parameters.solar_irradiance[channel];
      result.radiance[channel]+=path_transmittance[channel]*
          (direct+higher_order)*integral;
      path_transmittance[channel]*=segment_transmittance;
    }
  }
  result.transmittance=path_transmittance;
  return result;
}

AtmosphereNumericProbeValues atmosphere_numeric_probe_reference(
    const AtmosphereNumericProbeInput& input) {
  AtmosphereNumericProbeValues result{};
  const auto& parameters=input.parameters;
  if(validate_atmosphere(parameters))return result;
  const auto assign_spectrum=[&](std::size_t index,
                                 const AtmosphereSpectrum& spectrum){
    std::copy(spectrum.begin(),spectrum.end(),result[index].begin());
    result[index][3]=1.0;
  };
  const double height=parameters.atmosphere_height_metres;
  const double altitude=std::clamp(height*0.05,1.0,height-1.0);
  constexpr double cosine_angle=0.25;
  const auto transmittance_reference=[&](double sample_altitude,
                                         double sample_cosine){
    const tetra::Vec3 origin{
        0.0,parameters.ground_radius_metres+sample_altitude,0.0};
    const tetra::Vec3 direction=normalized({
        std::sqrt(std::max(0.0,1.0-sample_cosine*sample_cosine)),
        sample_cosine,0.0});
    AtmosphereSpectrum transmittance{1.0,1.0,1.0};
    if(const auto segment=atmosphere_ray_segment(origin,direction,parameters)){
      const auto start=origin+direction*segment->begin_metres;
      const auto end=origin+direction*segment->end_metres;
      transmittance=atmosphere_transmittance(start,end,parameters,512U);
    }
    return transmittance;
  };
  const auto transmittance=transmittance_reference(altitude,cosine_angle);
  assign_spectrum(0U,transmittance);
  assign_spectrum(1U,transmittance);

  const auto quality=atmosphere_quality_settings(input.quality);
  const auto multiple_size=static_cast<std::size_t>(
      quality.multiple_scattering_size);
  const auto lookup_coordinate=[&](double unit){
    return std::min(multiple_size-1U,static_cast<std::size_t>(
        std::floor(std::clamp(unit,0.0,1.0)*
                   static_cast<double>(multiple_size))));
  };
  const std::size_t multiple_x=lookup_coordinate(
      cosine_angle*0.5+0.5);
  const std::size_t multiple_y=lookup_coordinate(altitude/height);
  const double multiple_u=(static_cast<double>(multiple_x)+0.5)/
      static_cast<double>(multiple_size);
  const double multiple_v=(static_cast<double>(multiple_y)+0.5)/
      static_cast<double>(multiple_size);
  const double multiple_altitude=1.0+(height-2.0)*multiple_v;
  const double multiple_cosine=multiple_u*2.0-1.0;
  assign_spectrum(2U,atmosphere_multiple_scattering_reference(
      parameters,multiple_altitude,multiple_cosine,64U,20U).
          closed_contribution);

  const auto camera_forward=normalized(input.camera_forward);
  const auto sun_direction=normalized(input.sun_direction);
  const auto full_sky=atmosphere_scattering_reference(
      parameters,input.camera_position_from_planet_centre_metres,
      camera_forward,sun_direction,1.0e9,32U,64U,20U);
  assign_spectrum(3U,full_sky.radiance);

  const auto local_up=normalized(
      input.camera_position_from_planet_centre_metres);
  const auto irradiance=atmosphere_sky_irradiance_reference(
      local_up,local_up,[&](tetra::Vec3 direction){
        return atmosphere_scattering_reference(
            parameters,input.camera_position_from_planet_centre_metres,
            direction,sun_direction,1.0e9,32U,64U,20U).radiance;
      },64U);
  assign_spectrum(4U,irradiance);

  const double aerial_width=static_cast<double>(quality.aerial_width);
  const double aerial_height=static_cast<double>(quality.aerial_height);
  const auto aerial_x=quality.aerial_width/2U;
  const auto aerial_y=quality.aerial_height/2U;
  const AtmosphereLookupCoordinates aerial_uv{
      (static_cast<double>(aerial_x)+0.5)/aerial_width,
      (static_cast<double>(aerial_y)+0.5)/aerial_height};
  const double ndc_x=aerial_uv.u*2.0-1.0;
  const double ndc_y=aerial_uv.v*2.0-1.0;
  const auto aerial_direction=normalized(input.camera_forward+
      input.camera_right*(ndc_x*input.vertical_tangent*input.aspect_ratio)+
      input.camera_down*(ndc_y*input.vertical_tangent));
  const auto depth_minus_one=std::max(quality.aerial_depth-1U,1U);
  const auto aerial_z=static_cast<unsigned>(std::clamp<long long>(
      std::llround(std::cbrt(0.5)*static_cast<double>(depth_minus_one)),
      0LL,static_cast<long long>(quality.aerial_depth-1U)));
  const double aerial_slice=static_cast<double>(aerial_z)/
      static_cast<double>(depth_minus_one);
  const double aerial_distance=aerial_lut_distance(
      aerial_slice,input.maximum_aerial_distance_metres);
  const auto aerial=atmosphere_scattering_reference(
      parameters,input.camera_position_from_planet_centre_metres,
      aerial_direction,sun_direction,aerial_distance,32U,64U,20U);
  assign_spectrum(5U,aerial.radiance);
  assign_spectrum(6U,aerial.transmittance);

  const auto direct_aerial=atmosphere_scattering_reference(
      parameters,input.camera_position_from_planet_centre_metres,
      camera_forward,sun_direction,
      input.maximum_aerial_distance_metres*0.5,32U,64U,20U);
  assign_spectrum(7U,direct_aerial.radiance);
  assign_spectrum(8U,direct_aerial.transmittance);

  const auto uv=atmosphere_transmittance_uv(
      altitude,cosine_angle,parameters);
  const auto inverse=atmosphere_transmittance_parameters(uv,parameters);
  result[9U]={uv.u,uv.v,inverse.altitude_metres,inverse.zenith_cosine};
  const double one_metre_altitude=std::min(1.0,height);
  const double one_metre_radius=
      parameters.ground_radius_metres+one_metre_altitude;
  const double horizon_cosine=-std::sqrt(std::max(0.0,
      one_metre_altitude*(one_metre_radius+parameters.ground_radius_metres)/
          (one_metre_radius*one_metre_radius)));
  const std::array<std::pair<double,double>,
                   atmosphere_boundary_probe_case_count> boundary_cases{
      std::pair{0.0,1.0},std::pair{0.0,0.0},
      std::pair{one_metre_altitude,horizon_cosine},
      std::pair{height*0.5,0.0},std::pair{height*0.5,1.0},
      std::pair{height,-1.0},std::pair{height,1.0}};
  for(std::size_t case_index=0;case_index<boundary_cases.size();++case_index){
    const auto [case_altitude,case_cosine]=boundary_cases[case_index];
    const auto case_uv=atmosphere_transmittance_uv(
        case_altitude,case_cosine,parameters);
    const auto case_inverse=atmosphere_transmittance_parameters(
        case_uv,parameters);
    const std::size_t base=atmosphere_numeric_probe_base_value_count+
        case_index*3U;
    result[base]={case_uv.u,case_uv.v,case_inverse.altitude_metres,
                  case_inverse.zenith_cosine};
    // A transmittance table stores the physical ray represented by its mapped
    // coordinate. Below-horizon inputs clamp to the table boundary and are
    // separately rejected by sunlight visibility, so lookup and direct-ray
    // references intentionally differ there.
    assign_spectrum(base+1U,transmittance_reference(
        case_inverse.altitude_metres,case_inverse.zenith_cosine));
    assign_spectrum(base+2U,transmittance_reference(
        case_altitude,case_cosine));
  }
  return result;
}

AtmosphereNumericProbeReport evaluate_atmosphere_numeric_probe(
    const AtmosphereNumericProbeValues& actual,
    const AtmosphereNumericProbeInput& input) {
  AtmosphereNumericProbeReport report;
  report.reference=atmosphere_numeric_probe_reference(input);
  const std::array<std::string_view,
                   atmosphere_numeric_probe_base_value_count> base_names{
      "transmittance_lookup","transmittance_direct",
      "multiple_scattering_lookup","full_sky_lookup",
      "sky_irradiance_lookup","aerial_scattering_lookup",
      "aerial_transmittance_lookup","aerial_scattering_direct",
      "aerial_transmittance_direct","transmittance_mapping"};
  const std::array<double,
                   atmosphere_numeric_probe_base_value_count> base_absolute{
      2.0e-3,2.0e-3,2.0e-4,5.0e-4,7.5e-4,
      5.0e-4,3.0e-3,5.0e-4,3.0e-3,2.0e-4};
  const std::array<double,
                   atmosphere_numeric_probe_base_value_count> base_relative{
      0.02,0.02,0.15,0.15,0.20,0.20,0.03,0.15,0.03,0.0};
  const std::array<std::string_view,atmosphere_boundary_probe_case_count>
      boundary_names{"ground_up","ground_tangent","one_metre_horizon",
                     "mid_horizontal","mid_up","top_down","top_up"};
  report.passed=true;
  report.comparisons.reserve(atmosphere_numeric_probe_value_count);
  for(std::size_t index=0;index<atmosphere_numeric_probe_value_count;++index){
    AtmosphereNumericProbeComparison comparison;
    const bool base=index<atmosphere_numeric_probe_base_value_count;
    const std::size_t boundary_offset=base?0U:
        index-atmosphere_numeric_probe_base_value_count;
    const std::size_t boundary_component=boundary_offset%3U;
    const bool mapping=index==9U||(!base&&boundary_component==0U);
    if(base)comparison.name=base_names[index];
    else{
      comparison.name=std::string{boundary_names[boundary_offset/3U]}+
          (boundary_component==0U?"_mapping":
           boundary_component==1U?"_lookup":"_direct");
    }
    comparison.actual=actual[index];
    comparison.expected=report.reference[index];
    comparison.passed=true;
    for(std::size_t channel=0;channel<4U;++channel){
      const double expected=comparison.expected[channel];
      const double observed=comparison.actual[channel];
      comparison.absolute_error[channel]=std::abs(observed-expected);
      comparison.relative_error[channel]=comparison.absolute_error[channel]/
          std::max(std::abs(expected),1.0e-12);
      double absolute_tolerance=base?base_absolute[index]:
          (mapping?2.0e-4:2.0e-3);
      double relative_tolerance=base?base_relative[index]:
          (mapping?0.0:(boundary_component==1U?0.08:0.02));
      if(mapping&&channel==2U)
        absolute_tolerance=std::max(
            0.25,input.parameters.atmosphere_height_metres*2.0e-5);
      if(channel==3U&&!mapping){
        absolute_tolerance=1.0e-4;
        relative_tolerance=0.0;
      }
      const bool finite=std::isfinite(observed)&&std::isfinite(expected);
      comparison.passed&=finite&&comparison.absolute_error[channel]<=
          absolute_tolerance+relative_tolerance*std::abs(expected);
    }
    report.passed&=comparison.passed;
    report.comparisons.push_back(std::move(comparison));
  }
  return report;
}

double aerial_lut_distance(double slice,
                           double maximum_distance_metres) noexcept {
  if (!(maximum_distance_metres > 0.0) || !std::isfinite(slice) ||
      !std::isfinite(maximum_distance_metres))
    return 0.0;
  slice = std::clamp(slice, 0.0, 1.0);
  return slice * slice * slice * maximum_distance_metres;
}

double aerial_lut_slice(double distance_metres,
                        double maximum_distance_metres) noexcept {
  if (!(maximum_distance_metres > 0.0) || !std::isfinite(distance_metres) ||
      !std::isfinite(maximum_distance_metres))
    return 0.0;
  return std::cbrt(std::clamp(distance_metres / maximum_distance_metres,
                              0.0, 1.0));
}

double atmosphere_local_aerial_distance(
    const AtmosphereParameters& parameters, double camera_altitude_metres,
    double visible_distance_metres) noexcept {
  if(validate_atmosphere(parameters)||
     !(visible_distance_metres>0.0)||!std::isfinite(visible_distance_metres))
    return 0.0;
  camera_altitude_metres=std::isfinite(camera_altitude_metres)?
      camera_altitude_metres:0.0;
  // Eight scale heights retain more than 99.9% of either exponential medium.
  // The floor preserves useful ground-distance resolution for compact worlds;
  // the ceiling prevents a thin high-altitude medium from wasting the froxel
  // depth axis on orbital empty space. Longer paths use the explicit march.
  const double medium_extent=8.0*std::max(
      parameters.rayleigh_scale_height_metres,
      parameters.mie_scale_height_metres);
  const double altitude_headroom=std::clamp(camera_altitude_metres,0.0,
      parameters.atmosphere_height_metres);
  const double physical_extent=std::clamp(
      std::max(2'000.0,medium_extent+altitude_headroom),2'000.0,64'000.0);
  return std::min(visible_distance_metres,physical_extent);
}

double atmosphere_shadow_filter_visibility(
    std::size_t lit_samples, std::size_t sample_count,
    double footprint_fade) noexcept {
  if(sample_count==0U)return 1.0;
  const double filtered=static_cast<double>(std::min(lit_samples,sample_count))/
      static_cast<double>(sample_count);
  footprint_fade=std::clamp(footprint_fade,0.0,1.0);
  return 1.0+(filtered-1.0)*footprint_fade;
}

int run_atmosphere_check(AtmospherePreset preset, double camera_altitude,
                         double view_zenith_degrees,
                         double sun_zenith_degrees, std::ostream& output,
                         std::ostream& errors) {
  auto parameters = atmosphere_preset(preset);
  if (const auto issue = validate_atmosphere(parameters)) {
    errors << *issue << '\n';
    return 1;
  }
  if (!std::isfinite(camera_altitude) || camera_altitude < 0.0 ||
      !std::isfinite(view_zenith_degrees) ||
      !std::isfinite(sun_zenith_degrees)) {
    errors << "atmosphere camera and angles must be finite; altitude must be nonnegative\n";
    return 2;
  }
  const double view = view_zenith_degrees * std::numbers::pi / 180.0;
  const double sun = sun_zenith_degrees * std::numbers::pi / 180.0;
  const tetra::Vec3 position{0.0,
      parameters.ground_radius_metres + camera_altitude, 0.0};
  const tetra::Vec3 direction{std::sin(view), std::cos(view), 0.0};
  const auto segment = atmosphere_ray_segment(position, direction, parameters);
  if (!segment) {
    errors << "view ray does not cross the atmosphere\n";
    return 1;
  }
  const auto start = position + direction * segment->begin_metres;
  const auto end = position + direction * segment->end_metres;
  const auto transmittance = atmosphere_transmittance(start, end, parameters);
  const double cosine_angle = std::cos(view - sun);
  output << std::setprecision(12)
         << "{\"event\":\"atmosphere_check\",\"preset\":\""
         << atmosphere_preset_name(preset) << "\",\"parameter_hash\":"
         << atmosphere_parameter_hash(parameters) << ",\"camera_altitude_metres\":"
         << camera_altitude << ",\"view_zenith_degrees\":"
         << view_zenith_degrees << ",\"sun_zenith_degrees\":"
         << sun_zenith_degrees << ",\"path_metres\":"
         << segment->end_metres - segment->begin_metres
         << ",\"transmittance\":[" << transmittance[0] << ','
         << transmittance[1] << ',' << transmittance[2]
         << "],\"rayleigh_phase\":" << rayleigh_phase(cosine_angle)
         << ",\"mie_phase\":"
         << mie_henyey_greenstein_phase(cosine_angle,
                                                  parameters.mie_anisotropy)
         << "}\n";
  return 0;
}

}  // namespace tetra_viewer
