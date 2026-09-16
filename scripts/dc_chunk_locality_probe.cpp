#include "tetra_probes/sandwich_probe.hpp"

#include <iostream>

int main() {
  try {
    const auto report=tetra::probes::run_dual_chunk_locality_probe();
    std::cout<<tetra::probes::make_dual_chunk_locality_report_json(report);
    return report.local_outputs_match_monolithic&&report.reverse_request_order_independent&&
        report.remote_domain_growth_bounded ? 0 : 1;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 2;
  }
}
