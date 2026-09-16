#include "tetra_probes/dual_locality_probe.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments { unsigned int domain_radius{4U}; std::filesystem::path output; };

Arguments parse_arguments(int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--fixture") {
      if (++index == argc || std::string(argv[index]) != "uniform-root") throw std::invalid_argument("only --fixture uniform-root is supported");
    } else if (argument == "--domain-size") {
      if (++index == argc) throw std::invalid_argument("--domain-size requires a positive integer");
      result.domain_radius = static_cast<unsigned int>(std::stoul(argv[index]));
    } else if (argument == "--field") {
      if (++index == argc || std::string(argv[index]) != "all") throw std::invalid_argument("this R0 run requires --field all");
    } else if (argument == "--request") {
      if (++index == argc || std::string(argv[index]) != "fixed-cell-centre-cube")
        throw std::invalid_argument("only --request fixed-cell-centre-cube is supported");
    } else if (argument == "--output") {
      if (++index == argc) throw std::invalid_argument("--output requires a directory");
      result.output = argv[index];
    } else throw std::invalid_argument("unknown argument: " + argument);
  }
  if (result.domain_radius < 2U) throw std::invalid_argument("--domain-size must be at least 2");
  if (result.output.empty()) throw std::invalid_argument("--output is required");
  return result;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const std::vector reports{
        tetra::probes::run_dual_locality_probe(tetra::probes::LocalityField::plane, arguments.domain_radius),
        tetra::probes::run_dual_locality_probe(tetra::probes::LocalityField::sphere, arguments.domain_radius)};
    std::filesystem::create_directories(arguments.output);
    std::ofstream output(arguments.output / "report.json");
    if (!output) { std::cerr << "cannot write report.json\n"; return 5; }
    output << tetra::probes::make_dual_locality_report_json(reports);
    const bool passed = std::all_of(reports.begin(), reports.end(), [](const auto& report) {
      return report.classification == tetra::probes::LocalityClassification::bounded;
    });
    std::cout << "dual_locality_probe: " << (passed ? "bounded" : "percolating")
              << "; report=" << (arguments.output / "report.json") << '\n';
    return passed ? 0 : 4;
  } catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
