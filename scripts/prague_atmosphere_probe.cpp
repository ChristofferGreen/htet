// Deterministic matched-domain probe for the pinned Prague Sky Model used by
// scripts/compare_atmosphere_references.sh. This utility is compiled against
// the external reference checkout; it is not part of the runtime renderer.
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include "PragueSkyModelTest.h"

int main(int argc,char** argv) {
  if(argc!=6){
    std::cerr<<"usage: prague_atmosphere_probe DATASET SUN_ELEVATION "
                "VIEW_ELEVATION VIEW_AZIMUTH ALBEDO\n";
    return 2;
  }
  const std::string dataset=argv[1];
  const double sun_elevation=degreesToRadians(std::strtod(argv[2],nullptr));
  const double view_elevation=degreesToRadians(std::strtod(argv[3],nullptr));
  const double view_azimuth=degreesToRadians(std::strtod(argv[4],nullptr));
  const double albedo=std::strtod(argv[5],nullptr);
  PragueSkyModel model;
  model.initialize(dataset,59.4);
  const Vector3 viewpoint(0.0,0.0,0.0);
  const Vector3 direction(
      std::cos(view_elevation)*std::cos(view_azimuth),
      std::cos(view_elevation)*std::sin(view_azimuth),
      std::sin(view_elevation));
  const auto parameters=model.computeParameters(
      viewpoint,direction,sun_elevation,0.0,59.4,albedo);
  Spectrum spectrum{};
  for(std::size_t index=0;index<spectrum.size();++index)
    spectrum[index]=model.skyRadiance(parameters,SPECTRUM_WAVELENGTHS[index]);
  const Vector3 rgb=spectrumToRGB(spectrum);
  const double sum=std::max(rgb.x+rgb.y+rgb.z,1.0e-30);
  std::cout<<std::setprecision(12)
      <<"{\"sun_elevation_degrees\":"<<argv[2]
      <<",\"view_elevation_degrees\":"<<argv[3]
      <<",\"view_azimuth_degrees\":"<<argv[4]
      <<",\"linear_rgb\":["<<rgb.x<<','<<rgb.y<<','<<rgb.z<<']'
      <<",\"chromaticity\":["<<rgb.x/sum<<','<<rgb.y/sum<<','<<rgb.z/sum
      <<"]}\n";
}
