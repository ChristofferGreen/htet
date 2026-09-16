#include "tetra_probes/plc_chunk_probe.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  try {
    unsigned int radius = 6; std::filesystem::path output;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--surface") { if (++i == argc || std::string(argv[i]) != "frozen-plane") throw std::invalid_argument("only --surface frozen-plane is supported"); }
      else if (arg == "--request") { if (++i == argc || std::string(argv[i]) != "adjacent-fixed-rectangles") throw std::invalid_argument("only --request adjacent-fixed-rectangles is supported"); }
      else if (arg == "--outer-radius") { if (++i == argc) throw std::invalid_argument("--outer-radius requires a value"); radius = static_cast<unsigned int>(std::stoul(argv[i])); }
      else if (arg == "--boundary") { if (++i == argc || std::string(argv[i]) != "surface-edge-aligned") throw std::invalid_argument("only --boundary surface-edge-aligned is supported"); }
      else if (arg == "--output") { if (++i == argc) throw std::invalid_argument("--output requires a directory"); output = argv[i]; }
      else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (output.empty()) throw std::invalid_argument("--output is required");
    const auto report = tetra::probes::run_plc_chunk_probe(radius);
    std::filesystem::create_directories(output);
    std::ofstream(output / "report.json") << tetra::probes::make_plc_chunk_report_json(report);
    std::ofstream(output / "left_chunk.obj") << tetra::probes::make_plc_chunk_probe_obj(radius, true);
    std::ofstream(output / "right_chunk.obj") << tetra::probes::make_plc_chunk_probe_obj(radius, false);
    std::cout << "plc_chunk_probe: " << (report.bounded && report.adjacent_interface_identical ? "passed" : "failed") << "; report=" << (output / "report.json") << '\n';
    return report.bounded && report.adjacent_interface_identical ? 0 : 4;
  } catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
