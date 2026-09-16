#include "tetra_probes/sandwich_probe.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

bool parse_unsigned(std::string_view text, unsigned int& value) {
  const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
  return result.ec==std::errc{}&&result.ptr==text.data()+text.size();
}

bool parse_double(std::string_view text, double& value) {
  char* end{};
  const std::string owned{text};
  value=std::strtod(owned.c_str(),&end);
  return end==owned.c_str()+static_cast<std::ptrdiff_t>(owned.size());
}

bool parse_phase(std::string_view text, double& x, double& y) {
  const auto separator=text.find(':');
  return separator!=std::string_view::npos&&
      parse_double(text.substr(0U,separator),x)&&
      parse_double(text.substr(separator+1U),y);
}

} // namespace

int main(int argc,char** argv) {
  tetra::probes::SandwichConfig config;
  std::string report_path;
  std::string svg_path;
  std::string viewer_data_path;
  bool dual_method=true;
  for(int index=1;index<argc;++index) {
    const std::string_view argument{argv[index]};
    const auto value_after=[&](std::string_view prefix)->std::string_view {
      return argument.starts_with(prefix)?argument.substr(prefix.size()):std::string_view{};
    };
    if(const auto value=value_after("--resolution=");!value.empty()) {
      if(!parse_unsigned(value,config.resolution)) { std::cerr<<"invalid resolution\n"; return 2; }
    } else if(const auto value=value_after("--field=");!value.empty()) {
      if(value=="planar")config.field=tetra::probes::SandwichField::planar;
      else if(value=="perlin-height")config.field=tetra::probes::SandwichField::perlin_height;
      else { std::cerr<<"unknown field\n"; return 2; }
    } else if(const auto value=value_after("--method=");!value.empty()) {
      if(value=="dual")dual_method=true;
      else if(value=="marching-control")dual_method=false;
      else { std::cerr<<"unknown method\n"; return 2; }
    } else if(const auto value=value_after("--amplitude=");!value.empty()) {
      if(!parse_double(value,config.amplitude)) { std::cerr<<"invalid amplitude\n"; return 2; }
    } else if(const auto value=value_after("--frequency=");!value.empty()) {
      if(!parse_double(value,config.frequency)) { std::cerr<<"invalid frequency\n"; return 2; }
    } else if(const auto value=value_after("--phase=");!value.empty()) {
      if(!parse_phase(value,config.phase_x,config.phase_y)) { std::cerr<<"invalid phase\n"; return 2; }
    } else if(const auto value=value_after("--report=");!value.empty()) {
      report_path=std::string{value};
    } else if(const auto value=value_after("--svg=");!value.empty()) {
      svg_path=std::string{value};
    } else if(const auto value=value_after("--viewer-data=");!value.empty()) {
      viewer_data_path=std::string{value};
    } else {
      std::cerr<<"usage: tetra_sandwich_probe [--method=dual|marching-control] [--resolution=N] [--field=planar|perlin-height]"
                   " [--amplitude=A] [--frequency=F] [--phase=X:Y] [--report=path] [--svg=path]"
                   " [--viewer-data=path]\n";
      return 2;
    }
  }
  try {
    std::string json;
    bool selected_valid{};
    if(dual_method) {
      const auto report=tetra::probes::run_dual_contour_probe(config);
      json=tetra::probes::make_dual_contour_report_json(report);
      tetra::probes::SharedLatticeZipperRequest request;
      request.surface=tetra::probes::extract_frozen_dual_contour_surface(config);
      request.core=tetra::probes::extract_shared_lattice_regular_core(config);
      selected_valid=tetra::probes::construct_shared_lattice_zipper(request).accepted();
    } else {
      const auto report=tetra::probes::run_sandwich_probe(config);
      json=tetra::probes::make_sandwich_report_json(report);
      selected_valid=report.valid;
    }
    std::cout<<json;
    if(!report_path.empty()) {
      std::ofstream output{report_path};
      if(!output) { std::cerr<<"could not open report path\n"; return 2; }
      output<<json;
      if(!output) { std::cerr<<"could not write report path\n"; return 2; }
    }
    if(!svg_path.empty()) {
      std::ofstream output{svg_path};
      if(!output) { std::cerr<<"could not open SVG path\n"; return 2; }
      output<<tetra::probes::make_sandwich_svg(config);
      if(!output) { std::cerr<<"could not write SVG path\n"; return 2; }
    }
    if(!viewer_data_path.empty()) {
      std::ofstream output{viewer_data_path};
      if(!output) { std::cerr<<"could not open viewer-data path\n"; return 2; }
      output<<tetra::probes::make_sandwich_viewer_data(config);
      if(!output) { std::cerr<<"could not write viewer-data path\n"; return 2; }
    }
    return selected_valid?0:1;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 2;
  }
}
