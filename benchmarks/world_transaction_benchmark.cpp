#include "tetra_core/world_cut_directory.hpp"

#include <array>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
  std::cout<<std::fixed<<std::setprecision(3);
  for(const unsigned int width:{3U,4U,5U})
    for(unsigned int phase=0;phase<width;++phase){
      std::vector<tetra::WorldTetAddress> targets;
      auto address=tetra::WorldTetAddress::root(0U);
      const unsigned int depth=30U+phase;
      for(unsigned int level=0;level<depth;++level)
        address=address.child(static_cast<std::uint8_t>((level*7U)%8U));
      targets.push_back(address);
      tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
          targets,width,1U,tetra::HierarchyResidencyTier::conforming_volume));
      const auto target=targets[phase%targets.size()];
      const auto staged=directory.stage_transaction(
          {{{target,tetra::WorldTopologyOperation::split}}},2U);
      const auto& metrics=staged.transaction.metrics;
      std::cout<<"{\"width\":"<<width<<",\"phase\":"<<phase
               <<",\"owners\":"<<metrics.source_logical_owners
               <<",\"closure_edits\":"<<metrics.closure_edits
               <<",\"dependencies\":"<<metrics.dependency_reads
               <<",\"affected_blocks\":"<<metrics.affected_blocks
               <<",\"staged_bytes\":"<<metrics.staged_bytes
               <<",\"planning_ms\":"<<metrics.planning_milliseconds
               <<",\"closure_ms\":"<<metrics.closure_milliseconds
               <<",\"staging_ms\":"<<metrics.staging_milliseconds<<"}\n";
    }
}
