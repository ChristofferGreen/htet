#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/advancing_front_step.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc,char** argv) {
  std::string output_path;
  std::string step_output_path;
  for(int i=1;i<argc;++i) {
    const std::string_view argument{argv[i]};
    constexpr std::string_view prefix="--viewer-data=";
    if(argument.starts_with(prefix))output_path=std::string{argument.substr(prefix.size())};
    else if(argument.starts_with("--step-data="))
      step_output_path=std::string{argument.substr(std::string_view{"--step-data="}.size())};
    else {
      std::cerr<<"usage: advancing_front_fixture_probe [--viewer-data=path] [--step-data=path]\n";
      return 2;
    }
  }
  try {
    const auto fixture=tetra::probes::build_advancing_front_fixture();
    const auto data=tetra::probes::make_advancing_front_viewer_data(fixture);
    if(output_path.empty())std::cout<<data;
    else {
      std::ofstream output{output_path};
      if(!output) {std::cerr<<"could not open viewer data path\n";return 2;}
      output<<data;
      if(!output) {std::cerr<<"could not write viewer data\n";return 2;}
    }
    bool accepted=fixture.audit.accepted;
    if(!step_output_path.empty()) {
      const auto step=tetra::probes::advance_one_front_tetrahedron(fixture);
      std::ofstream output{step_output_path};
      if(!output) {std::cerr<<"could not open step data path\n";return 2;}
      output<<tetra::probes::make_advancing_front_step_viewer_data(step);
      if(!output) {std::cerr<<"could not write step data\n";return 2;}
      accepted=accepted&&step.audit.accepted;
    }
    return accepted?0:1;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 2;
  }
}
