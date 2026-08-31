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
  if (name == "reference-hillaire-2020")
    return AtmosphereTransport::reference_hillaire_2020;
  return std::nullopt;
}

std::string_view atmosphere_transport_name(
    AtmosphereTransport transport) noexcept {
  switch (transport) {
    case AtmosphereTransport::qualified_baseline:
      return "qualified-baseline";
    case AtmosphereTransport::faithful_hillaire:
      return "faithful-hillaire";
    case AtmosphereTransport::reference_hillaire_2020:
      return "reference-hillaire-2020";
  }
  return "qualified-baseline";
}

std::optional<AtmosphereRenderingMethod> parse_atmosphere_rendering_method(
    std::string_view name) {
  if(name=="current-qualified")
    return AtmosphereRenderingMethod::current_qualified;
  if(name=="native-screen-oracle")
    return AtmosphereRenderingMethod::native_screen_oracle;
  if(name=="deterministic-half-resolution")
    return AtmosphereRenderingMethod::deterministic_half_resolution;
  if(name=="temporal-half-resolution")
    return AtmosphereRenderingMethod::temporal_half_resolution;
  if(name=="deterministic-shadowed-froxels")
    return AtmosphereRenderingMethod::deterministic_shadowed_froxels;
  return std::nullopt;
}

std::string_view atmosphere_rendering_method_name(
    AtmosphereRenderingMethod method) noexcept {
  switch(method){
    case AtmosphereRenderingMethod::current_qualified:
      return "current-qualified";
    case AtmosphereRenderingMethod::native_screen_oracle:
      return "native-screen-oracle";
    case AtmosphereRenderingMethod::deterministic_half_resolution:
      return "deterministic-half-resolution";
    case AtmosphereRenderingMethod::temporal_half_resolution:
      return "temporal-half-resolution";
    case AtmosphereRenderingMethod::deterministic_shadowed_froxels:
      return "deterministic-shadowed-froxels";
  }
  return "current-qualified";
}

std::optional<AtmosphereShadowIntegrator> parse_atmosphere_shadow_integrator(
    std::string_view name) {
  if(name=="fixed-32")return AtmosphereShadowIntegrator::fixed_32;
  if(name=="adaptive-transition")
    return AtmosphereShadowIntegrator::adaptive_transition;
  if(name=="minmax-segments")
    return AtmosphereShadowIntegrator::minmax_segments;
  if(name=="dense-oracle")return AtmosphereShadowIntegrator::dense_oracle;
  if(name=="moment-hybrid")return AtmosphereShadowIntegrator::moment_hybrid;
  if(name=="epipolar-minmax")
    return AtmosphereShadowIntegrator::epipolar_minmax;
  return std::nullopt;
}

std::string_view atmosphere_shadow_integrator_name(
    AtmosphereShadowIntegrator integrator) noexcept {
  switch(integrator){
    case AtmosphereShadowIntegrator::fixed_32:return "fixed-32";
    case AtmosphereShadowIntegrator::adaptive_transition:
      return "adaptive-transition";
    case AtmosphereShadowIntegrator::minmax_segments:return "minmax-segments";
    case AtmosphereShadowIntegrator::dense_oracle:return "dense-oracle";
    case AtmosphereShadowIntegrator::moment_hybrid:return "moment-hybrid";
    case AtmosphereShadowIntegrator::epipolar_minmax:
      return "epipolar-minmax";
  }
  return "fixed-32";
}

std::size_t atmosphere_epipolar_minmax_element_count(
    std::size_t radial_resolution,std::size_t angular_rows) noexcept {
  std::size_t per_row{};
  while(radial_resolution>0U){
    per_row+=radial_resolution;
    if(radial_resolution==1U)break;
    radial_resolution=(radial_resolution+1U)/2U;
  }
  return per_row*angular_rows;
}

AtmosphereEpipolarLayout atmosphere_epipolar_layout(
    std::size_t shadow_resolution) noexcept {
  if(shadow_resolution==0U)return {};
  std::size_t shadow_capacity{};
  for(auto width=shadow_resolution;width>0U;width=(width+1U)/2U){
    shadow_capacity+=width*width;
    if(width==1U)break;
  }
  // The final two uvec2 entries are reserved for fence-safe GPU traversal
  // diagnostics while the buffer contains the epipolar hierarchy.
  if(shadow_capacity<=2U)return {};
  shadow_capacity-=2U;
  constexpr double tau=6.28318530717958647692;
  const auto fits=[&](std::size_t radial){
    const auto rows=static_cast<std::size_t>(
        std::ceil(tau*static_cast<double>(radial)));
    return atmosphere_epipolar_minmax_element_count(radial,rows)<=
        shadow_capacity;
  };
  std::size_t low=1U;
  std::size_t high=shadow_resolution;
  while(low<high){
    const auto middle=low+(high-low+1U)/2U;
    if(fits(middle))low=middle;
    else high=middle-1U;
  }
  const auto per_row=atmosphere_epipolar_minmax_element_count(low,1U);
  const auto rows=std::max<std::size_t>(1U,shadow_capacity/per_row);
  return {.radial_resolution=low,
          .angular_rows=rows,
          .element_count=per_row*rows};
}

std::size_t atmosphere_epipolar_row_from_angle(
    double angle_radians,std::size_t angular_rows) noexcept {
  if(angular_rows==0U||!std::isfinite(angle_radians))return 0U;
  constexpr double tau=6.28318530717958647692;
  double wrapped=std::fmod(angle_radians+3.14159265358979323846,tau);
  if(wrapped<0.0)wrapped+=tau;
  const auto row=static_cast<std::size_t>(
      wrapped/tau*static_cast<double>(angular_rows));
  return std::min(row,angular_rows-1U);
}

std::optional<SurfaceShadowBiasMode> parse_surface_shadow_bias_mode(
    std::string_view name) {
  if(name=="slope-scaled")return SurfaceShadowBiasMode::slope_scaled;
  if(name=="receiver-plane")return SurfaceShadowBiasMode::receiver_plane;
  return std::nullopt;
}

std::string_view surface_shadow_bias_mode_name(
    SurfaceShadowBiasMode mode) noexcept {
  switch(mode){
    case SurfaceShadowBiasMode::slope_scaled:return "slope-scaled";
    case SurfaceShadowBiasMode::receiver_plane:return "receiver-plane";
  }
  return "slope-scaled";
}

std::optional<AtmosphereShadowFilter> parse_atmosphere_shadow_filter(
    std::string_view name) {
  if(name=="unfiltered")return AtmosphereShadowFilter::unfiltered;
  if(name=="fixed-tent")return AtmosphereShadowFilter::fixed_tent;
  if(name=="physical-footprint")
    return AtmosphereShadowFilter::physical_footprint;
  return std::nullopt;
}

std::string_view atmosphere_shadow_filter_name(
    AtmosphereShadowFilter filter) noexcept {
  switch(filter){
    case AtmosphereShadowFilter::unfiltered:return "unfiltered";
    case AtmosphereShadowFilter::fixed_tent:return "fixed-tent";
    case AtmosphereShadowFilter::physical_footprint:return "physical-footprint";
  }
  return "fixed-tent";
}

std::uint64_t atmosphere_epipolar_source_revision(
    std::uint64_t surface_generation,const std::array<float,16>& matrix,
    float raster_bias_constant,float raster_bias_slope,
    AtmosphereShadowFilter filter,double comparison_bias_world) noexcept {
  std::uint64_t hash=1469598103934665603ULL;
  hash_double(hash,static_cast<double>(surface_generation));
  for(const float value:matrix)hash_double(hash,static_cast<double>(value));
  hash_double(hash,static_cast<double>(raster_bias_constant));
  hash_double(hash,static_cast<double>(raster_bias_slope));
  hash_double(hash,static_cast<double>(filter));
  hash_double(hash,comparison_bias_world);
  return hash;
}

bool compatible_atmosphere_shadow_generation(
    const AtmosphereShadowIntegrationGeneration& generation) noexcept {
  if(!generation.complete||generation.fallback)return false;
  if(generation.integrator==AtmosphereShadowIntegrator::minmax_segments||
     generation.integrator==AtmosphereShadowIntegrator::moment_hybrid||
     generation.integrator==AtmosphereShadowIntegrator::epipolar_minmax)
    return generation.shadow_depth_generation!=0U&&
        generation.hierarchy_generation==generation.shadow_depth_generation;
  return generation.hierarchy_generation==0U;
}

AtmosphereMomentVisibility atmosphere_moment_visibility(
    std::span<const double> blocker_depths,double receiver_depth,
    double residual_tolerance) {
  AtmosphereMomentVisibility result;
  if(blocker_depths.empty()||!std::isfinite(receiver_depth)||
     !(residual_tolerance>=0.0)||!std::isfinite(residual_tolerance))
    return result;
  std::array<double,5> moment{};
  moment[0]=1.0;
  std::size_t count{};
  for(const double depth:blocker_depths){
    if(!std::isfinite(depth))continue;
    const double value=std::clamp(depth,0.0,1.0);
    double power=value;
    for(std::size_t order=1;order<=4U;++order){
      moment[order]+=power;
      power*=value;
    }
    ++count;
  }
  if(count==0U)return result;
  for(std::size_t order=1;order<=4U;++order)
    moment[order]/=static_cast<double>(count);
  const double variance=moment[2]-moment[1]*moment[1];
  if(variance<=1.0e-12){
    result.estimate=receiver_depth<=moment[1]?1.0:0.0;
    result.lower=result.upper=result.estimate;
    result.residual=0.0;
    result.confident=true;
    return result;
  }
  // A measure supported on at most two depths obeys m(k+2)=s*m(k+1)-p*m(k).
  // Recover those two depths from the first three moments and use the fourth
  // exclusively as a confidence test. Three-layer distributions generally
  // violate the recurrence and therefore fall back to the min/max path.
  const double sum=(moment[3]-moment[1]*moment[2])/variance;
  const double product=sum*moment[1]-moment[2];
  const double discriminant=sum*sum-4.0*product;
  if(discriminant<0.0||!std::isfinite(discriminant))return result;
  const double root=std::sqrt(std::max(discriminant,0.0));
  const double first=0.5*(sum-root);
  const double second=0.5*(sum+root);
  if(first<-1.0e-6||second>1.0+1.0e-6||second-first<=1.0e-12)
    return result;
  const double first_weight=std::clamp(
      (second-moment[1])/(second-first),0.0,1.0);
  const double predicted_fourth=sum*moment[3]-product*moment[2];
  result.residual=std::abs(predicted_fourth-moment[4]);
  result.estimate=(receiver_depth<=first?first_weight:0.0)+
      (receiver_depth<=second?1.0-first_weight:0.0);
  const double uncertainty=std::clamp(
      result.residual/std::max(residual_tolerance,1.0e-15),0.0,1.0);
  result.lower=std::max(0.0,result.estimate-uncertainty);
  result.upper=std::min(1.0,result.estimate+uncertainty);
  result.confident=result.residual<=residual_tolerance;
  return result;
}

AtmosphereShadowIntegrationResult integrate_atmosphere_shadow(
    AtmosphereShadowIntegrator integrator,
    const AtmosphereShadowVisibilityFunction& visibility,
    const AtmosphereShadowWeightFunction& weight,
    const AtmosphereShadowRangeClassifier& classify_range,
    AtmosphereShadowIntegrationOptions options) {
  AtmosphereShadowIntegrationResult result;
  if(!visibility||options.base_intervals==0U||options.dense_intervals==0U||
     options.maximum_samples==0U)return result;
  const auto sample_weight=[&](double position){
    const double value=weight?weight(position):1.0;
    return std::isfinite(value)?std::max(0.0,value):0.0;
  };
  // The dense path is the qualification oracle.  Its requested interval
  // count must not be silently truncated by the interactive hierarchy work
  // budget, otherwise sufficiently narrow or late blockers disappear from
  // the reference itself.
  const std::size_t visibility_budget=
      integrator==AtmosphereShadowIntegrator::dense_oracle?
          options.dense_intervals:options.maximum_samples;
  const auto sample_visibility=[&](double position){
    if(result.visibility_samples>=visibility_budget){
      result.fallback=true;
      return 1.0;
    }
    ++result.visibility_samples;
    const double value=visibility(std::clamp(position,0.0,1.0));
    return std::isfinite(value)?std::clamp(value,0.0,1.0):1.0;
  };
  const auto emit=[&](double begin,double end,double visible){
    if(!(end>begin))return;
    result.intervals.push_back({begin,end,std::clamp(visible,0.0,1.0)});
  };

  if(integrator==AtmosphereShadowIntegrator::fixed_32||
     integrator==AtmosphereShadowIntegrator::dense_oracle){
    const std::size_t count=integrator==AtmosphereShadowIntegrator::fixed_32?
        options.base_intervals:options.dense_intervals;
    for(std::size_t index=0;index<count;++index){
      const double u0=static_cast<double>(index)/static_cast<double>(count);
      const double u1=static_cast<double>(index+1U)/static_cast<double>(count);
      const double begin=u0*u0;
      const double end=u1*u1;
      emit(begin,end,sample_visibility(0.5*(begin+end)));
    }
  }else if((integrator==AtmosphereShadowIntegrator::minmax_segments||
            integrator==AtmosphereShadowIntegrator::moment_hybrid||
            integrator==AtmosphereShadowIntegrator::epipolar_minmax)&&
           classify_range){
    const auto descend=[&](auto&& self,double begin,double end,
                           std::size_t depth)->void {
      ++result.visited_ranges;
      if(const auto uniform=classify_range(begin,end)){
        emit(begin,end,*uniform);
        return;
      }
      if(depth>=options.maximum_subdivision_depth||
         result.visibility_samples>=options.maximum_samples){
        if(result.visibility_samples>=options.maximum_samples)
          result.fallback=true;
        emit(begin,end,sample_visibility(0.5*(begin+end)));
        return;
      }
      const double middle=0.5*(begin+end);
      self(self,begin,middle,depth+1U);
      self(self,middle,end,depth+1U);
    };
    for(std::size_t index=0;index<options.base_intervals;++index){
      const double interval_count=static_cast<double>(options.base_intervals);
      const double u0=static_cast<double>(index)/interval_count;
      const double u1=static_cast<double>(index+1U)/interval_count;
      descend(descend,u0*u0,u1*u1,0U);
    }
  }else{
    const auto descend=[&](auto&& self,double begin,double end,
                           double at_begin,double at_middle,double at_end,
                           std::size_t depth)->void {
      ++result.visited_ranges;
      const double minimum=std::min({at_begin,at_middle,at_end});
      const double maximum=std::max({at_begin,at_middle,at_end});
      if(maximum-minimum<=options.transition_tolerance){
        emit(begin,end,at_middle);
        return;
      }
      if(depth>=options.maximum_subdivision_depth||
         result.visibility_samples+2U>options.maximum_samples){
        if(result.visibility_samples+2U>options.maximum_samples)
          result.fallback=true;
        // Conservative under the work cap: never invent sunlight across a
        // visibility discontinuity that was actually observed.
        emit(begin,end,minimum);
        return;
      }
      const double middle=0.5*(begin+end);
      const double left_middle=0.5*(begin+middle);
      const double right_middle=0.5*(middle+end);
      const double at_left=sample_visibility(left_middle);
      const double at_right=sample_visibility(right_middle);
      self(self,begin,middle,at_begin,at_left,at_middle,depth+1U);
      self(self,middle,end,at_middle,at_right,at_end,depth+1U);
    };
    for(std::size_t index=0;index<options.base_intervals;++index){
      const double interval_count=static_cast<double>(options.base_intervals);
      const double u0=static_cast<double>(index)/interval_count;
      const double u1=static_cast<double>(index+1U)/interval_count;
      const double begin=u0*u0;
      const double end=u1*u1;
      const double middle=0.5*(begin+end);
      descend(descend,begin,end,sample_visibility(begin),
              sample_visibility(middle),sample_visibility(end),0U);
    }
  }

  for(const auto& interval:result.intervals){
    const double weighted_length=
        sample_weight(0.5*(interval.begin+interval.end))*
        (interval.end-interval.begin);
    const double loss=1.0-interval.visibility;
    result.weighted_loss+=weighted_length*loss;
    result.total_weight+=weighted_length;
    result.maximum_loss=std::max(result.maximum_loss,loss);
  }
  return result;
}

AtmosphereShadowErrorMetrics atmosphere_shadow_error_metrics(
    std::span<const double> candidate,std::span<const double> reference,
    double sample_spacing) {
  AtmosphereShadowErrorMetrics result;
  if(candidate.empty()||candidate.size()!=reference.size()||
     !(sample_spacing>0.0)||!std::isfinite(sample_spacing)){
    result.root_mean_square_error=std::numeric_limits<double>::infinity();
    result.maximum_error=std::numeric_limits<double>::infinity();
    result.boundary_distance=std::numeric_limits<double>::infinity();
    result.gradient_step_energy=std::numeric_limits<double>::infinity();
    return result;
  }
  double squared_error{};
  for(std::size_t index=0;index<candidate.size();++index){
    const double error=candidate[index]-reference[index];
    squared_error+=error*error;
    result.maximum_error=std::max(result.maximum_error,std::abs(error));
    if(index>0U){
      const double gradient_error=(candidate[index]-candidate[index-1U])-
          (reference[index]-reference[index-1U]);
      result.gradient_step_energy+=gradient_error*gradient_error;
    }
  }
  result.root_mean_square_error=
      std::sqrt(squared_error/static_cast<double>(candidate.size()));
  const auto boundaries=[](std::span<const double> values){
    std::vector<double> found;
    for(std::size_t index=1;index<values.size();++index)
      if((values[index-1U]<0.5)!=(values[index]<0.5))
        found.push_back(static_cast<double>(index)-0.5);
    return found;
  };
  const auto candidate_boundaries=boundaries(candidate);
  const auto reference_boundaries=boundaries(reference);
  if(candidate_boundaries.empty()&&reference_boundaries.empty())
    result.boundary_distance=0.0;
  else if(candidate_boundaries.empty()||reference_boundaries.empty())
    result.boundary_distance=
        sample_spacing*static_cast<double>(candidate.size());
  else{
    double total{};
    for(const double boundary:reference_boundaries){
      double closest=std::numeric_limits<double>::infinity();
      for(const double other:candidate_boundaries)
        closest=std::min(closest,std::abs(boundary-other));
      total+=closest;
    }
    result.boundary_distance=sample_spacing*total/
        static_cast<double>(reference_boundaries.size());
  }
  result.gradient_step_energy/=static_cast<double>(
      std::max<std::size_t>(1U,candidate.size()-1U));
  return result;
}

AtmosphereShadowDepthHierarchy make_atmosphere_shadow_depth_hierarchy(
    std::span<const double> depth,std::size_t width,std::size_t height,
    double clear_depth) {
  AtmosphereShadowDepthHierarchy hierarchy;
  if(width==0U||height==0U||depth.size()!=width*height||
     !std::isfinite(clear_depth))return hierarchy;
  AtmosphereShadowDepthLevel base;
  base.width=width;
  base.height=height;
  base.ranges.reserve(depth.size());
  for(double value:depth){
    if(!std::isfinite(value))value=clear_depth;
    value=std::clamp(value,0.0,1.0);
    base.ranges.push_back({value,value});
  }
  hierarchy.levels.push_back(std::move(base));
  while(width>1U||height>1U){
    const auto& previous=hierarchy.levels.back();
    AtmosphereShadowDepthLevel next;
    next.width=(width+1U)/2U;
    next.height=(height+1U)/2U;
    next.ranges.resize(next.width*next.height);
    for(std::size_t y=0;y<next.height;++y)
      for(std::size_t x=0;x<next.width;++x){
        AtmosphereShadowDepthRange range{1.0,0.0};
        for(std::size_t dy=0;dy<2U;++dy)
          for(std::size_t dx=0;dx<2U;++dx){
            const std::size_t child_x=x*2U+dx;
            const std::size_t child_y=y*2U+dy;
            if(child_x>=width||child_y>=height)continue;
            const auto child=previous.ranges[child_y*width+child_x];
            range.minimum=std::min(range.minimum,child.minimum);
            range.maximum=std::max(range.maximum,child.maximum);
          }
        next.ranges[y*next.width+x]=range;
      }
    width=next.width;
    height=next.height;
    hierarchy.levels.push_back(std::move(next));
  }
  return hierarchy;
}

AtmosphereEpipolarDepthHierarchy make_atmosphere_epipolar_depth_hierarchy(
    std::span<const double> depth,std::size_t radial_resolution,
    std::size_t angular_rows,double clear_depth) {
  AtmosphereEpipolarDepthHierarchy hierarchy;
  if(radial_resolution==0U||angular_rows==0U||
     depth.size()!=radial_resolution*angular_rows||
     !std::isfinite(clear_depth))return hierarchy;
  hierarchy.radial_resolution=radial_resolution;
  hierarchy.angular_rows=angular_rows;
  AtmosphereEpipolarDepthLevel base{
      .width=radial_resolution,.rows=angular_rows};
  base.ranges.reserve(depth.size());
  for(double value:depth){
    if(!std::isfinite(value))value=clear_depth;
    value=std::clamp(value,0.0,1.0);
    base.ranges.push_back({value,value});
  }
  hierarchy.levels.push_back(std::move(base));
  std::size_t width=radial_resolution;
  while(width>1U){
    const auto& previous=hierarchy.levels.back();
    AtmosphereEpipolarDepthLevel next{
        .width=(width+1U)/2U,.rows=angular_rows};
    next.ranges.resize(next.width*angular_rows);
    for(std::size_t row=0;row<angular_rows;++row)
      for(std::size_t x=0;x<next.width;++x){
        auto range=previous.ranges[row*width+x*2U];
        if(x*2U+1U<width){
          const auto second=previous.ranges[row*width+x*2U+1U];
          range.minimum=std::min(range.minimum,second.minimum);
          range.maximum=std::max(range.maximum,second.maximum);
        }
        next.ranges[row*next.width+x]=range;
      }
    width=next.width;
    hierarchy.levels.push_back(std::move(next));
  }
  return hierarchy;
}

AtmosphereEpipolarTraversalResult traverse_atmosphere_epipolar_row(
    const AtmosphereEpipolarDepthHierarchy& hierarchy,std::size_t row,
    double radial_begin,double radial_end,double receiver_depth_begin,
    double receiver_depth_end,double comparison_bias,
    std::size_t maximum_intervals) {
  AtmosphereEpipolarTraversalResult result;
  if(hierarchy.levels.empty()||row>=hierarchy.angular_rows||
     maximum_intervals==0U||!std::isfinite(radial_begin)||
     !std::isfinite(radial_end)||!std::isfinite(receiver_depth_begin)||
     !std::isfinite(receiver_depth_end)||!std::isfinite(comparison_bias)||
     radial_begin==radial_end){
    result.fallback=true;
    return result;
  }
  const double radial_delta=radial_end-radial_begin;
  const double receiver_delta=receiver_depth_end-receiver_depth_begin;
  const auto emit=[&](double begin,double end,double visibility){
    if(!(end>begin))return;
    if(!result.intervals.empty()&&
       std::abs(result.intervals.back().end-begin)<=1.0e-12&&
       result.intervals.back().visibility==visibility){
      result.intervals.back().end=end;
      return;
    }
    if(result.intervals.size()>=maximum_intervals){
      result.fallback=true;
      return;
    }
    result.intervals.push_back({begin,end,visibility});
  };
  const auto visit=[&](auto&& self,std::size_t level,std::size_t node)->void {
    if(result.fallback)return;
    std::size_t scale=1U;
    for(std::size_t index=0;index<level;++index)scale*=2U;
    const double node_begin=static_cast<double>(node*scale)/
        static_cast<double>(hierarchy.radial_resolution);
    const double node_end=static_cast<double>(std::min(
        (node+1U)*scale,hierarchy.radial_resolution)) /
        static_cast<double>(hierarchy.radial_resolution);
    double t0=(node_begin-radial_begin)/radial_delta;
    double t1=(node_end-radial_begin)/radial_delta;
    if(t1<t0)std::swap(t0,t1);
    t0=std::clamp(t0,0.0,1.0);
    t1=std::clamp(t1,0.0,1.0);
    if(!(t1>t0))return;
    ++result.visited_nodes;
    const auto range=hierarchy.levels[level].ranges[
        row*hierarchy.levels[level].width+node];
    const double receiver0=receiver_depth_begin+receiver_delta*t0;
    const double receiver1=receiver_depth_begin+receiver_delta*t1;
    const double receiver_min=std::min(receiver0,receiver1)-comparison_bias;
    const double receiver_max=std::max(receiver0,receiver1)-comparison_bias;
    if(receiver_max<=range.minimum){emit(t0,t1,1.0);return;}
    if(receiver_min>range.maximum){emit(t0,t1,0.0);return;}
    if(level==0U){
      if(std::abs(range.maximum-range.minimum)<=1.0e-12&&
         std::abs(receiver_delta)>1.0e-15){
        const double crossing=(range.minimum+comparison_bias-
            receiver_depth_begin)/receiver_delta;
        if(crossing>t0&&crossing<t1){
          const bool begins_lit=receiver0-comparison_bias<=range.minimum;
          emit(t0,crossing,begins_lit?1.0:0.0);
          emit(crossing,t1,begins_lit?0.0:1.0);
          return;
        }
        const double middle=0.5*(t0+t1);
        const double middle_receiver=receiver_depth_begin+
            receiver_delta*middle-comparison_bias;
        emit(t0,t1,middle_receiver<=range.minimum?1.0:0.0);
        return;
      }
      result.fallback=true;
      return;
    }
    const std::size_t first=node*2U;
    const std::size_t child_width=hierarchy.levels[level-1U].width;
    if(radial_delta>0.0){
      self(self,level-1U,first);
      if(first+1U<child_width)self(self,level-1U,first+1U);
    }else{
      if(first+1U<child_width)self(self,level-1U,first+1U);
      self(self,level-1U,first);
    }
  };
  const std::size_t top=hierarchy.levels.size()-1U;
  const std::size_t top_width=hierarchy.levels[top].width;
  if(radial_delta>0.0){
    for(std::size_t node=0;node<top_width;++node)visit(visit,top,node);
  }else{
    for(std::size_t node=top_width;node>0U;--node)visit(visit,top,node-1U);
  }
  return result;
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
  const bool integrator=
      previous->shadow_integrator!=next.shadow_integrator;
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
          optical||scattering||sun||position||(baseline&&orientation)||
          integrator||shadow||origin,
      .long_shadow=transport==AtmosphereTransport::faithful_hillaire&&
          (optical||scattering||sun||position||integrator||shadow||origin),
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
  // The reference transport evaluates its view-dependent atmosphere directly
  // every frame. Record that live generation even when dynamic-sun mode skips
  // the retained legacy aerial and long-shadow lookups.
  if(dispatch.aerial_perspective||dispatch.long_shadow||
     material.transport==AtmosphereTransport::reference_hillaire_2020)
    result.view=AtmosphereViewLookupSnapshot{
        next.optical,next.scattering,next.sun,next.camera_position,
        next.camera_orientation,next.shadow_integrator,next.shadow,
        next.render_origin,
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
          (material.transport==AtmosphereTransport::qualified_baseline&&
           lookups.view->camera_orientation!=frame.camera_orientation)||
          lookups.view->shadow_integrator!=frame.shadow_integrator||
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
      // Preserve Earth's integrated aerosol optical depth in a compact but
      // vertically resolvable gameplay boundary layer. A 30 m profile made
      // the golden horizon switch almost completely over a few metres of
      // camera ascent. 300 m keeps readable kilometre-scale haze while making
      // the density change gradual at interactive flight speeds.
      result.mie_scale_height_metres = 300.0;
      result.mie_scattering_per_metre = {
          15.984e-6, 15.984e-6, 15.984e-6};
      result.mie_absorption_per_metre = {
          1.616e-6, 1.616e-6, 1.616e-6};
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

AtmosphereParameters adapt_compact_atmosphere_to_relief(
    AtmosphereParameters parameters,double maximum_relief_metres) noexcept {
  if(!(maximum_relief_metres>0.0)||!std::isfinite(maximum_relief_metres))
    return parameters;

  // A summit should retain at least 75% of datum-level molecular density.
  // This is an artistic compact-planet constraint layered on Hillaire's
  // altitude-only spherical medium, not a terrain-following atmosphere.
  constexpr double minimum_summit_density=0.75;
  const double required_scale_height=maximum_relief_metres/
      -std::log(minimum_summit_density);
  const double previous_scale_height=parameters.rayleigh_scale_height_metres;
  const double next_scale_height=std::max(
      previous_scale_height,required_scale_height);
  if(next_scale_height>previous_scale_height){
    const double optical_depth_scale=previous_scale_height/next_scale_height;
    for(double& coefficient:parameters.rayleigh_scattering_per_metre)
      coefficient*=optical_depth_scale;
    parameters.rayleigh_scale_height_metres=next_scale_height;
  }

  // Eight molecular scale heights leave less than 0.04% of datum density at
  // the top boundary.  Include the relief envelope explicitly so no terrain
  // can approach the numerical end of the participating segment.
  parameters.atmosphere_height_metres=std::max(
      parameters.atmosphere_height_metres,
      maximum_relief_metres+8.0*parameters.rayleigh_scale_height_metres);
  return parameters;
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

std::optional<AtmosphereReducedEndpoint> reduce_atmosphere_endpoint_2x2(
    const std::array<float,4>& reversed_depth,
    double near_plane_metres,std::uint32_t generation) noexcept {
  if(!(near_plane_metres>0.0)||!std::isfinite(near_plane_metres))
    return std::nullopt;
  float nearest_depth=0.0F;
  float farthest_opaque_depth=1.0F;
  std::size_t opaque_count{};
  for(const float depth:reversed_depth){
    if(!std::isfinite(depth)||depth<0.0F||depth>1.0F)
      return std::nullopt;
    nearest_depth=std::max(nearest_depth,depth);
    if(depth>1.0e-8F){
      ++opaque_count;
      farthest_opaque_depth=std::min(farthest_opaque_depth,depth);
    }
  }
  if(nearest_depth<=1.0e-8F)
    return AtmosphereReducedEndpoint{
        .classification=AtmosphereEndpointClass::sky,
        .reversed_depth=0.0F,
        .linear_depth_metres=0.0,
        .transition_confidence=1.0,
        .generation=generation};
  const double depth_spread=opaque_count>1U?
      1.0-static_cast<double>(farthest_opaque_depth)/nearest_depth:0.0;
  const double class_confidence=opaque_count==4U?1.0:0.0;
  return AtmosphereReducedEndpoint{
      .classification=AtmosphereEndpointClass::opaque,
      .reversed_depth=nearest_depth,
      .linear_depth_metres=near_plane_metres/
          static_cast<double>(nearest_depth),
      .transition_confidence=class_confidence*
          (1.0-std::clamp(depth_spread/0.05,0.0,1.0)),
      .generation=generation};
}

std::array<double,4> atmosphere_reconstruction_weights(
    AtmosphereEndpointClass target_class,double target_linear_depth_metres,
    const std::array<AtmosphereReducedEndpoint,4>& taps,
    const std::array<double,4>& bilinear_weights,
    double relative_depth_tolerance) noexcept {
  std::array<double,4> result{};
  if(!std::isfinite(relative_depth_tolerance)||
     !(relative_depth_tolerance>0.0))return result;
  if(target_class==AtmosphereEndpointClass::opaque&&
     (!std::isfinite(target_linear_depth_metres)||
      !(target_linear_depth_metres>0.0)))return result;
  double sum{};
  for(std::size_t index=0;index<result.size();++index){
    const auto& tap=taps[index];
    const double bilinear=bilinear_weights[index];
    if(tap.classification!=target_class||!std::isfinite(bilinear)||
       !(bilinear>0.0))continue;
    double depth_weight=1.0;
    if(target_class==AtmosphereEndpointClass::opaque){
      if(!std::isfinite(tap.linear_depth_metres)||
         !(tap.linear_depth_metres>0.0))continue;
      const double threshold=std::max(
          target_linear_depth_metres*relative_depth_tolerance,1.0e-6);
      const double difference=std::abs(
          tap.linear_depth_metres-target_linear_depth_metres);
      if(difference>=threshold)continue;
      depth_weight=1.0-difference/threshold;
    }
    result[index]=bilinear*depth_weight;
    sum+=result[index];
  }
  if(sum>0.0)
    for(auto& weight:result)weight/=sum;
  return result;
}

AtmosphereHistoryCompatibility atmosphere_screen_history_compatibility(
    const AtmosphereScreenHistoryIdentity& previous,
    const AtmosphereScreenHistoryIdentity& current) noexcept {
  using Reason=AtmosphereHistoryInvalidation;
  auto mask=std::uint32_t{};
  const auto add=[&](Reason reason){mask|=static_cast<std::uint32_t>(reason);};
  if(!previous.valid||!current.valid)add(Reason::uninitialized);
  if(previous.revisions.optical!=current.revisions.optical)
    add(Reason::optical);
  if(previous.revisions.scattering!=current.revisions.scattering)
    add(Reason::scattering);
  if(previous.revisions.sun!=current.revisions.sun)add(Reason::sun);
  if(previous.revisions.shadow_integrator!=
     current.revisions.shadow_integrator)add(Reason::shadow_integrator);
  if(previous.revisions.shadow!=current.revisions.shadow)add(Reason::shadow);
  if(previous.terrain_generation!=current.terrain_generation)
    add(Reason::terrain);
  if(previous.transport!=current.transport||
     previous.rendering_method!=current.rendering_method||
     previous.linear_resolution_divisor!=current.linear_resolution_divisor||
     previous.sample_count!=current.sample_count)
    add(Reason::representation);
  if(previous.width!=current.width||previous.height!=current.height)
    add(Reason::extent);
  return {
      .invalidation_mask=mask,
      .camera_changed=
          previous.revisions.camera_position!=current.revisions.camera_position||
          previous.revisions.sky_position!=current.revisions.sky_position||
          previous.revisions.camera_orientation!=
              current.revisions.camera_orientation,
      .render_origin_changed=previous.revisions.render_origin!=
      current.revisions.render_origin};
}

std::optional<AtmosphereReprojectedEndpoint> reproject_atmosphere_endpoint(
    const AtmosphereReprojectionCamera& current,
    const AtmosphereReprojectionCamera& previous,double current_u,
    double current_v,AtmosphereEndpointClass classification,
    double current_linear_depth_metres) noexcept {
  const auto finite_vector=[](tetra::Vec3 value){
    return std::isfinite(value.x)&&std::isfinite(value.y)&&
        std::isfinite(value.z);
  };
  if(!finite_vector(current.position_from_planet_centre_metres)||
     !finite_vector(previous.position_from_planet_centre_metres)||
     !finite_vector(current.right)||!finite_vector(current.down)||
     !finite_vector(current.forward)||!finite_vector(previous.right)||
     !finite_vector(previous.down)||!finite_vector(previous.forward)||
     !std::isfinite(current_u)||!std::isfinite(current_v)||
     !(current.tangent_x>0.0)||!(current.tangent_y>0.0)||
     !(previous.tangent_x>0.0)||!(previous.tangent_y>0.0)||
     (classification==AtmosphereEndpointClass::opaque&&
      !(current_linear_depth_metres>0.0)))return std::nullopt;
  auto direction=current.forward+
      current.right*((current_u*2.0-1.0)*current.tangent_x)+
      current.down*((current_v*2.0-1.0)*current.tangent_y);
  const double direction_length=length(direction);
  if(!(direction_length>0.0)||!std::isfinite(direction_length))
    return std::nullopt;
  direction=direction/direction_length;
  tetra::Vec3 previous_vector=direction;
  double expected_depth{};
  if(classification==AtmosphereEndpointClass::opaque){
    const auto point=current.position_from_planet_centre_metres+
        direction*current_linear_depth_metres;
    previous_vector=point-previous.position_from_planet_centre_metres;
    expected_depth=length(previous_vector);
  }
  const double forward=dot(previous_vector,previous.forward);
  if(!(forward>1.0e-9)||!std::isfinite(forward))return std::nullopt;
  const double u=0.5+0.5*dot(previous_vector,previous.right)/
      (forward*previous.tangent_x);
  const double v=0.5+0.5*dot(previous_vector,previous.down)/
      (forward*previous.tangent_y);
  if(!(u>=0.0&&u<1.0&&v>=0.0&&v<1.0))return std::nullopt;
  return AtmosphereReprojectedEndpoint{
      .u=u,.v=v,.previous_linear_depth_metres=expected_depth};
}

bool atmosphere_history_sample_compatible(
    const AtmosphereReducedEndpoint& current,
    const AtmosphereReducedEndpoint& previous,
    double expected_previous_depth_metres,
    std::uint32_t expected_previous_generation,
    bool reprojection_inside_view,double relative_depth_tolerance) noexcept {
  if(!reprojection_inside_view||current.classification!=previous.classification||
     current.generation==0U||previous.generation!=expected_previous_generation||
     !(current.transition_confidence>0.5)||
     !(previous.transition_confidence>0.5)||
     !(relative_depth_tolerance>0.0)||
     !std::isfinite(relative_depth_tolerance))return false;
  if(current.classification==AtmosphereEndpointClass::sky)return true;
  if(!(expected_previous_depth_metres>0.0)||
     !std::isfinite(expected_previous_depth_metres)||
     !(previous.linear_depth_metres>0.0)||
     !std::isfinite(previous.linear_depth_metres))return false;
  return std::abs(previous.linear_depth_metres-
                  expected_previous_depth_metres)<
      expected_previous_depth_metres*relative_depth_tolerance;
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

AtmosphereLookupCoordinates atmosphere_sun_focused_sky_uv(
    tetra::Vec3 direction,tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept {
  auto uv=atmosphere_full_sky_uv(direction,local_up,sun_direction);
  const double perimeter=(uv.u-0.5)*4.0;
  const double focused=std::copysign(
      std::sqrt(std::abs(perimeter)*0.5),perimeter);
  uv.u=focused*0.5+0.5;
  return uv;
}

tetra::Vec3 atmosphere_sun_focused_sky_direction(
    AtmosphereLookupCoordinates uv,tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept {
  uv.u=std::clamp(std::isfinite(uv.u)?uv.u:0.5,0.0,1.0);
  const double focused=uv.u*2.0-1.0;
  const double perimeter=std::copysign(
      2.0*focused*focused,focused);
  uv.u=perimeter*0.25+0.5;
  return atmosphere_full_sky_direction(uv,local_up,sun_direction);
}

AtmosphereLookupCoordinates atmosphere_sun_shadow_sky_uv(
    tetra::Vec3 direction,tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept {
  auto uv=atmosphere_full_sky_uv(direction,local_up,sun_direction);
  const double perimeter=(uv.u-0.5)*4.0;
  constexpr double focus_limit=0.25;
  constexpr double focused_span=0.25;
  constexpr double outer_scale=focused_span/(2.0-focus_limit);
  const double magnitude=std::abs(perimeter);
  const double focused=magnitude<=focus_limit?magnitude:
      focused_span+(magnitude-focus_limit)*outer_scale;
  uv.u=0.5+std::copysign(focused,perimeter);
  return uv;
}

tetra::Vec3 atmosphere_sun_shadow_sky_direction(
    AtmosphereLookupCoordinates uv,tetra::Vec3 local_up,
    tetra::Vec3 sun_direction) noexcept {
  uv.u=std::clamp(std::isfinite(uv.u)?uv.u:0.5,0.0,1.0);
  constexpr double focus_limit=0.25;
  constexpr double focused_span=0.25;
  constexpr double outer_inverse_scale=
      (2.0-focus_limit)/focused_span;
  const double focused=uv.u-0.5;
  const double magnitude=std::abs(focused);
  const double perimeter=magnitude<=focused_span?magnitude:
      focus_limit+(magnitude-focused_span)*outer_inverse_scale;
  uv.u=std::copysign(perimeter,focused)*0.25+0.5;
  return atmosphere_full_sky_direction(uv,local_up,sun_direction);
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

AtmosphereScatteringComponents atmosphere_scattering_components_reference(
    const AtmosphereParameters& parameters, tetra::Vec3 origin,
    tetra::Vec3 view_direction, tetra::Vec3 sun_direction,
    double maximum_distance,
    const AtmosphereTerrainVisibilityFunction& terrain_visibility,
    std::size_t view_steps,
    std::size_t multiple_direction_count,
    std::size_t multiple_ray_steps) {
  AtmosphereScatteringComponents result;
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
    const double raw_visibility=terrain_visibility?
        terrain_visibility(point):1.0;
    const double visibility=std::isfinite(raw_visibility)?
        std::clamp(raw_visibility,0.0,1.0):0.0;
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
          parameters.solar_irradiance[channel]*visibility;
      const double higher_order=(rayleigh_scattering+mie_scattering)*
          multiple[channel]*parameters.solar_irradiance[channel];
      result.direct_single_scattering[channel]+=
          path_transmittance[channel]*direct*integral;
      result.multiple_scattering[channel]+=
          path_transmittance[channel]*higher_order*integral;
      path_transmittance[channel]*=segment_transmittance;
    }
  }
  result.transmittance=path_transmittance;
  return result;
}

AtmosphereScatteringReference atmosphere_scattering_reference(
    const AtmosphereParameters& parameters, tetra::Vec3 origin,
    tetra::Vec3 view_direction, tetra::Vec3 sun_direction,
    double maximum_distance, std::size_t view_steps,
    std::size_t multiple_direction_count,
    std::size_t multiple_ray_steps) {
  const auto components=atmosphere_scattering_components_reference(
      parameters,origin,view_direction,sun_direction,maximum_distance,{},
      view_steps,multiple_direction_count,multiple_ray_steps);
  AtmosphereScatteringReference result{
      .transmittance=components.transmittance};
  for(std::size_t channel=0;channel<result.radiance.size();++channel)
    result.radiance[channel]=components.direct_single_scattering[channel]+
        components.multiple_scattering[channel];
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
  // The faithful aerial volume is stored in the same sun-focused full-sky
  // domain as the shader, not in screen UV. Qualify the addressed texel's
  // actual direction; camera-forward transport is covered independently by
  // the direct probe below.
  const auto aerial_direction=atmosphere_sun_focused_sky_direction(
      aerial_uv,local_up,sun_direction);
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

tetra::Vec3 clamp_atmosphere_camera_to_medium(
    tetra::Vec3 position, const AtmosphereParameters& parameters,
    double minimum_altitude_metres) noexcept {
  const double minimum_radius=parameters.ground_radius_metres+
      std::max(0.0,std::isfinite(minimum_altitude_metres)?
                       minimum_altitude_metres:0.0);
  const double radius=std::sqrt(
      position.x*position.x+position.y*position.y+position.z*position.z);
  if(std::isfinite(radius)&&radius>=minimum_radius)return position;
  if(std::isfinite(radius)&&radius>1.0e-12){
    const double scale=minimum_radius/radius;
    return position*scale;
  }
  return {0.0,minimum_radius,0.0};
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
