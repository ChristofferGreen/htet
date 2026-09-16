#include "tetra_probes/sandwich_probe.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool parse_unsigned(std::string_view text,unsigned int& value) {
  const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
  return result.ec==std::errc{}&&result.ptr==text.data()+text.size();
}

bool parse_double(std::string_view text,double& value) {
  const std::string owned{text};char* end{};
  value=std::strtod(owned.c_str(),&end);
  return end==owned.c_str()+static_cast<std::ptrdiff_t>(owned.size());
}

bool parse_phase(std::string_view text,double& x,double& y) {
  const auto separator=text.find(':');
  return separator!=std::string_view::npos&&parse_double(text.substr(0U,separator),x)&&
      parse_double(text.substr(separator+1U),y);
}

} // namespace

int main(int argc,char** argv) {
  tetra::probes::SandwichConfig config;
  for(int index=1;index<argc;++index) {
    const std::string_view argument{argv[index]};
    if(argument.starts_with("--resolution=")) {
      if(!parse_unsigned(argument.substr(13U),config.resolution))return 2;
    } else if(argument.starts_with("--phase=")) {
      if(!parse_phase(argument.substr(8U),config.phase_x,config.phase_y))return 2;
    } else if(argument.starts_with("--field=")) {
      const auto field=argument.substr(8U);
      if(field=="planar")config.field=tetra::probes::SandwichField::planar;
      else if(field=="perlin-height")config.field=tetra::probes::SandwichField::perlin_height;
      else return 2;
    } else {
      std::cerr<<"usage: dc_chunk_interface_probe [--resolution=N] [--field=planar|perlin-height] [--phase=X:Y]\n";
      return 2;
    }
  }
  try {
    const auto report=tetra::probes::run_dual_chunk_interface_probe(config);
    std::cout<<tetra::probes::make_dual_chunk_interface_report_json(report);
    // Success means the interface preconditions pass.  It deliberately does
    // not mean a chunk-local shell tetrahedralizer exists.
    return report.whole_triangle_ownership&&report.halo_positions_identical&&
        report.chunk_results_generated_independently&&report.canonical_surface_partition&&
        report.assembly_order_and_permutation_independent&&report.canonical_seam_edge_ids&&
        report.automatic_retained_core_selection&&report.retained_core_partition_independent&&
        report.retained_core_interface_paired?0:1;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 2;
  }
}
