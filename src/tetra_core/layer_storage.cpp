#include "tetra_core/layer_storage.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <tuple>

namespace tetra {
namespace {

using Clock=std::chrono::steady_clock;

std::uint64_t hash_addresses(std::span<const TetId> addresses){
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto address:addresses){hash^=address;hash*=1099511628211ULL;}
  return hash;
}

std::uint64_t macro_key(TetId address){
  return (static_cast<std::uint64_t>(tet_depth(address))<<56U)|
      (static_cast<std::uint64_t>(tet_root(address))<<40U)|(tet_path(address)>>6U);
}

std::uint64_t morton_key(const TetMesh& mesh,TetId address){
  Vec3 centre{};
  for(const VertexId vertex:mesh.tetrahedron(address).vertices)
    centre=centre+mesh.vertices()[vertex];
  centre=centre/4.0;
  const auto quantize=[](double value){
    return static_cast<std::uint32_t>(std::clamp(value,0.0,1.0)*1023.0+0.5);
  };
  const auto spread=[](std::uint32_t value){
    std::uint64_t result{};
    for(std::uint32_t bit=0;bit<10U;++bit)
      result|=static_cast<std::uint64_t>((value>>bit)&1U)<<(bit*3U);
    return result;
  };
  return spread(quantize(centre.x))|(spread(quantize(centre.y))<<1U)|
      (spread(quantize(centre.z))<<2U);
}

} // namespace

SupercubeLocation rsb_supercube_location(Vec3 central_vertex,unsigned int depth){
  SupercubeLocation result;
  result.level=depth/3U;
  const unsigned int exponent=std::min(30U,result.level+1U);
  const std::int64_t scale=std::int64_t{1}<<exponent;
  std::array<std::int64_t,3> coordinate{{
      std::llround(central_vertex.x*static_cast<double>(scale)),
      std::llround(central_vertex.y*static_cast<double>(scale)),
      std::llround(central_vertex.z*static_cast<double>(scale))}};
  std::array<unsigned int,3> local{};
  unsigned int odd_count{};
  for(std::size_t axis=0;axis<3U;++axis){
    result.block[axis]=static_cast<std::int32_t>(coordinate[axis]/4);
    auto remainder=coordinate[axis]%4;
    if(remainder<0){remainder+=4;--result.block[axis];}
    local[axis]=static_cast<unsigned int>(remainder);
    odd_count+=local[axis]&1U;
  }
  if(odd_count==0U)return result;
  result.diamond_class=static_cast<std::uint8_t>(3U-odd_count);
  std::uint8_t rank{};
  for(unsigned int x=0;x<4U;++x)
    for(unsigned int y=0;y<4U;++y)
      for(unsigned int z=0;z<4U;++z){
        const unsigned int candidate=(x&1U)+(y&1U)+(z&1U);
        if(candidate!=odd_count)continue;
        if(std::array<unsigned int,3>{{x,y,z}}==local){
          const std::uint8_t offset=odd_count==3U?0U:odd_count==2U?8U:32U;
          result.slot=static_cast<std::uint8_t>(offset+rank);
          result.valid=result.slot<56U;
          return result;
        }
        ++rank;
      }
  return result;
}

LayerStorageExperiment build_layer_storage_experiment(
    const TetMesh& mesh,const Sphere& field,LayerStorage storage,
    KernelOrder kernel_order) {
  const auto conversion_start=Clock::now();
  LayerStorageExperiment result;
  result.storage=storage;
  result.kernel_order=kernel_order;
  for(const auto& layer:mesh.layers())
    for(const auto& record:layer.tetrahedra)
      if(record.transition_parent==invalid_tet)
        result.canonical_addresses.push_back(record.address);
  std::sort(result.canonical_addresses.begin(),result.canonical_addresses.end());
  result.canonical_addresses.erase(std::unique(result.canonical_addresses.begin(),
                                                result.canonical_addresses.end()),
                                   result.canonical_addresses.end());
  result.traversal_addresses=result.canonical_addresses;

  if(storage==LayerStorage::mutable_macro_blocks||
     storage==LayerStorage::occupancy_bit_macro_blocks){
    for(const TetId address:result.canonical_addresses){
      const auto key=macro_key(address);
      if(result.blocks.empty()||result.blocks.back().key!=key){
        PackedMacroBlock block;
        block.key=key;
        block.mutable_slots.fill(invalid_tet);
        block.compact_begin=static_cast<std::uint32_t>(
            result.compact_block_addresses.size());
        result.blocks.push_back(block);
      }
      auto& block=result.blocks.back();
      const auto slot=static_cast<std::size_t>(tet_path(address)&63U);
      block.mutable_slots[slot]=address;
      block.occupancy|=std::uint64_t{1}<<slot;
      result.compact_block_addresses.push_back(address);
      ++block.compact_count;
    }
  }else if(storage==LayerStorage::address_runs){
    std::sort(result.traversal_addresses.begin(),result.traversal_addresses.end(),
              [&](TetId left,TetId right){
                const auto left_key=morton_key(mesh,left),right_key=morton_key(mesh,right);
                return left_key<right_key||(left_key==right_key&&left<right);
              });
    auto canonical=result.canonical_addresses;
    for(std::size_t begin=0;begin<canonical.size();){
      std::size_t end=begin+1U;
      while(end<canonical.size()&&canonical[end]==canonical[end-1U]+1U&&
            end-begin<std::numeric_limits<std::uint32_t>::max())++end;
      result.address_runs.push_back(
          {canonical[begin],static_cast<std::uint32_t>(end-begin)});
      begin=end;
    }
  }

  if(mesh.subdivision_method()==SubdivisionMethod::maubach_diamond){
    struct Diamond { unsigned int depth{};std::array<VertexId,2> edge{}; };
    std::vector<Diamond> diamonds;
    for(const auto& layer:mesh.layers())
      for(const auto& record:layer.tetrahedra)
        if(record.transition_parent==invalid_tet){
          const auto type=static_cast<std::size_t>(tet_refinement_type(record.address));
          std::array<VertexId,2> edge{{record.vertices[type],record.vertices[3]}};
          if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
          diamonds.push_back({tet_depth(record.address),edge});
        }
    std::sort(diamonds.begin(),diamonds.end(),[](const auto& left,const auto& right){
      return std::tie(left.depth,left.edge)<std::tie(right.depth,right.edge);
    });
    diamonds.erase(std::unique(diamonds.begin(),diamonds.end(),[](const auto& left,
                                                                  const auto& right){
      return left.depth==right.depth&&left.edge==right.edge;
    }),diamonds.end());
    result.metrics.source_rsb_diamonds=diamonds.size();
    struct Entry { SupercubeLocation location{}; };
    std::vector<Entry> entries;
    entries.reserve(diamonds.size());
    for(const auto& diamond:diamonds){
      const Vec3 centre=(mesh.vertices()[diamond.edge[0]]+
                         mesh.vertices()[diamond.edge[1]])/2.0;
      const auto location=rsb_supercube_location(centre,diamond.depth);
      if(location.valid)entries.push_back({location});
      else ++result.metrics.invalid_supercube_diamonds;
    }
    std::sort(entries.begin(),entries.end(),[](const auto& left,const auto& right){
      return std::tie(left.location.level,left.location.block,left.location.slot)<
             std::tie(right.location.level,right.location.block,right.location.slot);
    });
    for(const auto& entry:entries){
      if(result.supercubes.empty()||
         result.supercubes.back().level!=entry.location.level||
         result.supercubes.back().block!=entry.location.block)
        result.supercubes.push_back(
            {entry.location.level,entry.location.block,0U});
      result.supercubes.back().occupancy|=
          std::uint64_t{1}<<entry.location.slot;
    }
    result.metrics.supercube_diamonds=entries.size();
  }

  if(kernel_order==KernelOrder::orientation_buckets){
    std::stable_sort(result.traversal_addresses.begin(),result.traversal_addresses.end(),
        [](TetId left,TetId right){
          const auto left_orientation=tet_path(left)&7U;
          const auto right_orientation=tet_path(right)&7U;
          return left_orientation<right_orientation||
              (left_orientation==right_orientation&&left<right);
        });
  }else if(kernel_order==KernelOrder::fused_macro_blocks){
    std::stable_sort(result.traversal_addresses.begin(),result.traversal_addresses.end(),
        [](TetId left,TetId right){
          const auto left_key=macro_key(left),right_key=macro_key(right);
          return left_key<right_key||(left_key==right_key&&left<right);
        });
  }

  result.metrics.conversion_ms=std::chrono::duration<double,std::milli>(
      Clock::now()-conversion_start).count();
  result.metrics.block_count=result.blocks.size();
  result.metrics.address_run_count=result.address_runs.size();
  result.metrics.supercube_count=result.supercubes.size();
  if(!result.blocks.empty()){
    result.metrics.minimum_block_occupancy=64U;
    std::size_t total{};
    for(const auto& block:result.blocks){
      const auto occupancy=static_cast<std::size_t>(std::popcount(block.occupancy));
      result.metrics.minimum_block_occupancy=std::min(
          result.metrics.minimum_block_occupancy,occupancy);
      result.metrics.maximum_block_occupancy=std::max(
          result.metrics.maximum_block_occupancy,occupancy);
      total+=occupancy;
    }
    result.metrics.mean_block_occupancy=
        static_cast<double>(total)/static_cast<double>(result.blocks.size());
  }
  result.metrics.live_bytes=result.canonical_addresses.size()*sizeof(TetId)+
      result.blocks.size()*sizeof(PackedMacroBlock)+
      result.compact_block_addresses.size()*sizeof(TetId)+
      result.address_runs.size()*sizeof(PackedAddressRun)+
      result.supercubes.size()*sizeof(PackedSupercube);
  result.metrics.retained_bytes=result.canonical_addresses.capacity()*sizeof(TetId)+
      result.traversal_addresses.capacity()*sizeof(TetId)+
      result.blocks.capacity()*sizeof(PackedMacroBlock)+
      result.compact_block_addresses.capacity()*sizeof(TetId)+
      result.address_runs.capacity()*sizeof(PackedAddressRun)+
      result.supercubes.capacity()*sizeof(PackedSupercube);
  result.metrics.topology_hash=hash_addresses(result.canonical_addresses);

  const auto classification_start=Clock::now();
  std::vector<std::pair<TetId,SurfaceRelation>> classified;
  classified.reserve(result.traversal_addresses.size());
  for(const TetId address:result.traversal_addresses)
    classified.push_back({address,classify_tetrahedron(mesh,address,field)});
  std::sort(classified.begin(),classified.end(),[](const auto& left,const auto& right){
    return left.first<right.first;
  });
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& [address,relation]:classified){
    hash^=address;hash*=1099511628211ULL;
    hash^=static_cast<std::uint8_t>(relation);hash*=1099511628211ULL;
  }
  result.metrics.classification_hash=hash;
  result.metrics.classification_ms=std::chrono::duration<double,std::milli>(
      Clock::now()-classification_start).count();
  const double seconds=result.metrics.classification_ms/1000.0;
  if(seconds>0.0){
    result.metrics.candidate_throughput_per_second=
        static_cast<double>(classified.size())/seconds;
    result.metrics.exact_field_throughput_per_second=
        4.0*static_cast<double>(classified.size())/seconds;
  }
  return result;
}

} // namespace tetra
