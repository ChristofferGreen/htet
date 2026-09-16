#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/advancing_front_fill.hpp"

#include <iostream>
#include <fstream>
#include <string>

int main(int argc,char** argv) {
  tetra::probes::AdvancingFrontFixtureConfig config;
  config.noise_amplitude = 0.0;
  const auto fixture = tetra::probes::build_advancing_front_fixture(config);
  tetra::probes::AdvancingFrontFillOptions options;
  options.maximum_steps = argc>1?static_cast<std::size_t>(std::stoull(argv[1])):5000U;
  options.existing_candidate_limit = 16U;
  options.recovery_candidate_limit = 128U;
  options.maximum_rollbacks = 0U;
  const auto fill = tetra::probes::fill_advancing_front_cavity(fixture, options);
  if(argc>2){std::ofstream replay(argv[2]);replay<<tetra::probes::make_advancing_front_replay_data(fill);}
  std::cout << "dc_vertices=" << fixture.dc_vertices.size()
            << " boundary_faces=" << fixture.outer_triangles.size()
            << " core_tets=" << fixture.core_tetrahedra.size()
            << " core_faces=" << fixture.core_boundary_triangles.size()
            << " cavity_volume=" << fixture.audit.cavity_volume
            << " fixture_accepted=" << fixture.audit.accepted
            << " fill_accepted=" << fill.audit.accepted
            << " positive=" << fill.audit.positive_tetrahedra
            << " overlap_free=" << fill.audit.no_strict_overlap
            << " frozen=" << fill.audit.frozen_faces_preserved_partial
            << " inserted=" << fill.tetrahedra.size()
            << " remaining_faces=" << fill.audit.remaining_faces
            << " remaining_components=" << fill.audit.unresolved_components
            << " residual_sheets_closed=" << fill.audit.residual_sheets_closed
            << " remaining_volume=" << fill.audit.remaining_volume
            << " obstruction=" << fill.audit.obstruction_found
            << " atomic_joins=" << fill.audit.atomic_join_transactions
            << " pocket_repairs=" << fill.audit.pocket_repairs
            << " pocket_expansions=" << fill.audit.pocket_expansions
            << " kernel_search_failures=" << fill.audit.kernel_search_failures
            << " rejected=" << fill.audit.rejected_candidates
            << " reject_small=" << fill.audit.rejected_small_volume
            << " reject_overlap=" << fill.audit.rejected_overlap
            << " reject_crossing=" << fill.audit.rejected_front_crossing
            << " reject_incidence=" << fill.audit.rejected_front_incidence
            << " rollbacks=" << fill.audit.rollbacks << '\n';
  return fill.audit.accepted ? 0 : 1;
}
