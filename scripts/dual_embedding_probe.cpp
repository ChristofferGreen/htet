#include "tetra_probes/dual_embedding_probe.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  try {
    std::uint64_t seed = 0x5eed1234ULL; std::size_t budget = 512; std::filesystem::path output;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--surface") { if (++i == argc || std::string(argv[i]) != "strict-dual") throw std::invalid_argument("only --surface strict-dual is supported"); }
      else if (arg == "--field") { if (++i == argc || std::string(argv[i]) != "adversarial-trilinear") throw std::invalid_argument("only --field adversarial-trilinear is supported"); }
      else if (arg == "--seed") { if (++i == argc) throw std::invalid_argument("--seed requires a value"); seed = std::stoull(argv[i]); }
      else if (arg == "--budget") { if (++i == argc) throw std::invalid_argument("--budget requires a value"); budget = std::stoull(argv[i]); }
      else if (arg == "--output") { if (++i == argc) throw std::invalid_argument("--output requires a directory"); output = argv[i]; }
      else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (output.empty()) throw std::invalid_argument("--output is required");
    const auto report = tetra::probes::run_dual_embedding_probe(seed, budget);
    std::filesystem::create_directories(output);
    std::ofstream(output / "report.json") << tetra::probes::make_dual_embedding_report_json(report);
    std::cout << "dual_embedding_probe: " << (report.strict_dual_survived_search ? "no counterexample in bounded search" : "counterexample found") << "; report=" << (output / "report.json") << '\n';
    return report.strict_dual_survived_search ? 0 : 4;
  } catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
