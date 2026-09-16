#include "tetra_probes/dual_embedding_probe.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  std::uint64_t seed = 91U;
  std::size_t budget = 4096U;
  std::filesystem::path output;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--seed" && ++i < argc) seed = std::stoull(argv[i]);
    else if (argument == "--budget" && ++i < argc) budget = std::stoull(argv[i]);
    else if (argument == "--output" && ++i < argc) output = argv[i];
    else throw std::invalid_argument("usage: --seed N --budget N --output DIR");
  }
  if (output.empty()) throw std::invalid_argument("--output is required");
  const auto report = tetra::probes::run_strict_dual_placement_probe(seed, budget);
  std::filesystem::create_directories(output);
  std::ofstream(output / "report.json") <<
      tetra::probes::make_strict_dual_placement_report_json(report);
  std::cout << report.conclusion << '\n';
  // A mask-29 realization is only a local control.  The probe succeeds only
  // when the declared fixture corpus survives under the candidate rule.
  return report.safe_rule_survived_corpus ? 0 : 4;
}
