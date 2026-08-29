// Deterministic RGB probe for Eric Bruneton's pinned double-precision CPU
// reference solver. It is compiled only by the external qualification script.
#include "atmosphere/reference/model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace atmosphere::reference;
using dimensional::vec3;

namespace {

AtmosphereParameters earth_parameters() {
  constexpr int minimum_wavelength=360;
  constexpr int maximum_wavelength=830;
  std::vector<SpectralIrradiance> solar;
  std::vector<ScatteringCoefficient> rayleigh;
  std::vector<ScatteringCoefficient> mie_scattering;
  std::vector<ScatteringCoefficient> mie_extinction;
  std::vector<ScatteringCoefficient> absorption;
  const auto absorption_coefficient=[](double wavelength){
    // Piecewise-linear spectral extension through the renderer's B/G/R ozone
    // coefficients. The comparison samples those exact three wavelengths.
    if(wavelength<=440.0)return 0.085e-6;
    if(wavelength<=550.0)
      return 0.085e-6+(1.881e-6-0.085e-6)*(wavelength-440.0)/110.0;
    if(wavelength<=680.0)
      return 1.881e-6+(0.650e-6-1.881e-6)*(wavelength-550.0)/130.0;
    return 0.650e-6;
  };
  for(int wavelength=minimum_wavelength;wavelength<=maximum_wavelength;
      wavelength+=10){
    const double micrometres=static_cast<double>(wavelength)*1.0e-3;
    solar.push_back(1.0*watt_per_square_meter_per_nm);
    rayleigh.push_back(1.24062e-6*std::pow(micrometres,-4.0)/m);
    mie_scattering.push_back(3.996e-6/m);
    mie_extinction.push_back(4.4e-6/m);
    absorption.push_back(absorption_coefficient(wavelength)/m);
  }
  AtmosphereParameters result;
  result.solar_irradiance=IrradianceSpectrum(
      minimum_wavelength*nm,maximum_wavelength*nm,solar);
  result.sun_angular_radius=0.2678*deg;
  result.bottom_radius=6360.0*km;
  result.top_radius=6460.0*km;
  result.rayleigh_density.layers[1]=DensityProfileLayer(
      0.0*m,1.0,-1.0/(8000.0*m),0.0/m,0.0);
  result.rayleigh_scattering=ScatteringSpectrum(
      minimum_wavelength*nm,maximum_wavelength*nm,rayleigh);
  result.mie_density.layers[1]=DensityProfileLayer(
      0.0*m,1.0,-1.0/(1200.0*m),0.0/m,0.0);
  result.mie_scattering=ScatteringSpectrum(
      minimum_wavelength*nm,maximum_wavelength*nm,mie_scattering);
  result.mie_extinction=ScatteringSpectrum(
      minimum_wavelength*nm,maximum_wavelength*nm,mie_extinction);
  result.mie_phase_function_g=0.8;
  result.absorption_density.layers[0]=DensityProfileLayer(
      25.0*km,0.0,0.0/km,1.0/(15.0*km),-2.0/3.0);
  result.absorption_density.layers[1]=DensityProfileLayer(
      0.0*km,0.0,0.0/km,-1.0/(15.0*km),8.0/3.0);
  result.absorption_extinction=ScatteringSpectrum(
      minimum_wavelength*nm,maximum_wavelength*nm,absorption);
  result.ground_albedo=DimensionlessSpectrum(0.1);
  result.mu_s_min=cos(102.0*deg);
  return result;
}

}  // namespace

int main(int argc,char** argv) {
  if(argc!=6){
    std::cerr<<"usage: bruneton_atmosphere_probe CACHE SUN_ELEVATION "
                "VIEW_ELEVATION VIEW_AZIMUTH ORDERS\n";
    return 2;
  }
  const Angle sun_elevation=std::strtod(argv[2],nullptr)*deg;
  const Angle view_elevation=std::strtod(argv[3],nullptr)*deg;
  const Angle view_azimuth=std::strtod(argv[4],nullptr)*deg;
  const unsigned orders=static_cast<unsigned>(std::strtoul(argv[5],nullptr,10));
  Model model(earth_parameters(),argv[1]);
  model.Init(orders);
  const Position camera=Position(0.0*m,0.0*m,6360.0*km+50.0*m);
  const Direction direction=Direction(
      cos(view_elevation)*cos(view_azimuth),
      cos(view_elevation)*sin(view_azimuth),
      sin(view_elevation));
  const Direction sun=Direction(
      cos(sun_elevation),0.0,sin(sun_elevation));
  DimensionlessSpectrum transmittance;
  const auto radiance=model.GetSkyRadiance(
      camera,direction,0.0*m,sun,&transmittance);
  const auto value=[&](double wavelength){
    return radiance(wavelength*nm).to(watt_per_square_meter_per_sr_per_nm);
  };
  const std::array<double,3> rgb{{value(680.0),value(550.0),value(440.0)}};
  const double sum=std::max(rgb[0]+rgb[1]+rgb[2],1.0e-30);
  std::cout<<std::setprecision(12)
      <<"{\"sun_elevation_degrees\":"<<argv[2]
      <<",\"view_elevation_degrees\":"<<argv[3]
      <<",\"view_azimuth_degrees\":"<<argv[4]
      <<",\"linear_rgb\":["<<rgb[0]<<','<<rgb[1]<<','<<rgb[2]<<']'
      <<",\"chromaticity\":["<<rgb[0]/sum<<','<<rgb[1]/sum<<','
      <<rgb[2]/sum<<"]}\n";
}
