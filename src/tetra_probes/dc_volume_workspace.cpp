#include "tetra_probes/dc_volume_workspace.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <new>

namespace tetra::probes {
namespace {
thread_local DcVolumeBuildWorkspace* active_workspace{};
thread_local DcVolumeBuildWorkspace* most_recent_workspace{};
std::mutex registry_mutex;
DcVolumeBuildWorkspace* registry_head{};

DcVolumeBuildWorkspace* registered_workspace_for(const void* pointer) noexcept {
  std::lock_guard lock(registry_mutex);
  for(auto* workspace=registry_head;workspace;workspace=workspace->registry_next())
    if(workspace->owns(pointer))return workspace;
  return nullptr;
}

void* system_allocate(std::size_t bytes,std::size_t alignment) {
  void* result{};
  if(alignment<=alignof(std::max_align_t))result=std::malloc(bytes==0U?1U:bytes);
  else if(posix_memalign(&result,alignment,bytes==0U?1U:bytes)!=0)result=nullptr;
  if(!result)throw std::bad_alloc{};
  return result;
}
} // namespace

struct alignas(64) DcVolumeBuildWorkspace::Block {
  Block* next{};
  std::size_t size{};
  bool free{};
};

void* dc_volume_workspace_allocate(std::size_t bytes,std::size_t alignment) {
  if(active_workspace)return active_workspace->allocate(bytes,alignment);
  return system_allocate(bytes,alignment);
}

bool dc_volume_workspace_owns(const void* pointer) noexcept {
  return registered_workspace_for(pointer)!=nullptr;
}

void dc_volume_workspace_deallocate(void* pointer) noexcept {
  if(!pointer)return;
  if(active_workspace&&active_workspace->owns(pointer))active_workspace->deallocate(pointer);
  else if(most_recent_workspace&&most_recent_workspace->owns(pointer))most_recent_workspace->deallocate(pointer);
  else if(auto* workspace=registered_workspace_for(pointer))workspace->deallocate(pointer);
  else std::free(pointer);
}

DcVolumeBuildWorkspace::DcVolumeBuildWorkspace(std::size_t bytes)
    :capacity_(bytes) {
  if(bytes<128U||(bytes&(bytes-1U))!=0U)throw std::bad_alloc{};
  if(posix_memalign(reinterpret_cast<void**>(&storage_),bytes,bytes)!=0||!storage_)
    throw std::bad_alloc{};
  maximum_order_=0U;for(auto size=bytes;size>1U;size>>=1U)++maximum_order_;
  if(maximum_order_>=maximum_orders_) { std::free(storage_);throw std::bad_alloc{}; }
  reset();
  std::lock_guard lock(registry_mutex);
  registry_next_=registry_head;registry_head=this;
}

DcVolumeBuildWorkspace::~DcVolumeBuildWorkspace() {
  std::lock_guard lock(registry_mutex);
  auto** cursor=&registry_head;
  while(*cursor&&*cursor!=this)cursor=&(*cursor)->registry_next_;
  if(*cursor)*cursor=registry_next_;
  if(active_workspace==this)active_workspace=nullptr;
  if(most_recent_workspace==this)most_recent_workspace=nullptr;
  std::free(storage_);
}

void DcVolumeBuildWorkspace::reset() noexcept {
  if(active_)return;
  free_lists_.fill(nullptr);used_=0U;live_bytes_=0U;allocations_=0U;exhausted_=false;
  auto* root=reinterpret_cast<Block*>(storage_);
  root->next=nullptr;root->size=capacity_;root->free=true;
  free_lists_[maximum_order_]=root;
}

void* DcVolumeBuildWorkspace::allocate(std::size_t bytes,std::size_t alignment) {
  // The 64-byte header makes ordinary and over-aligned standard-library
  // allocations naturally aligned. The bounded probe does not use alignment
  // beyond a cache line; refuse larger exotic requests instead of heap fallback.
  if(alignment>64U||bytes>capacity_-sizeof(Block)) { exhausted_=true;throw std::bad_alloc{}; }
  std::size_t required=bytes+sizeof(Block);std::size_t order=7U;
  for(std::size_t block_size=128U;block_size<required;block_size<<=1U)++order;
  std::size_t available=order;
  while(available<=maximum_order_&&!free_lists_[available])++available;
  if(available>maximum_order_) { exhausted_=true;throw std::bad_alloc{}; }
  auto* block=free_lists_[available];free_lists_[available]=block->next;
  while(available>order) {
    --available;const auto half=static_cast<std::size_t>(1ULL<<available);
    auto* buddy=reinterpret_cast<Block*>(reinterpret_cast<std::byte*>(block)+half);
    buddy->next=free_lists_[available];buddy->size=half;buddy->free=true;
    free_lists_[available]=buddy;block->size=half;
  }
  block->next=nullptr;block->free=false;block->size=static_cast<std::size_t>(1ULL<<order);
  live_bytes_+=block->size;used_=std::max(used_,live_bytes_);++allocations_;
  return reinterpret_cast<std::byte*>(block)+sizeof(Block);
}

void DcVolumeBuildWorkspace::deallocate(void* pointer) noexcept {
  auto* block=reinterpret_cast<Block*>(reinterpret_cast<std::byte*>(pointer)-sizeof(Block));
  if(block->free)return;
  live_bytes_-=block->size;block->free=true;
  std::size_t order=0U;for(auto size=block->size;size>1U;size>>=1U)++order;
  block->next=free_lists_[order];free_lists_[order]=block;
}

bool DcVolumeBuildWorkspace::owns(const void* pointer) const noexcept {
  const auto value=reinterpret_cast<std::uintptr_t>(pointer);
  const auto low=reinterpret_cast<std::uintptr_t>(storage_);
  return storage_&&value>=low&&value<low+capacity_;
}

DcVolumeBuildWorkspaceScope::DcVolumeBuildWorkspaceScope(DcVolumeBuildWorkspace& workspace) noexcept
    :previous_(active_workspace),workspace_(&workspace) {
  if(workspace.active_)return;
  workspace.active_=true;active_workspace=&workspace;active_=true;
}

DcVolumeBuildWorkspaceScope::~DcVolumeBuildWorkspaceScope() {
  if(!active_)return;
  workspace_->active_=false;most_recent_workspace=workspace_;active_workspace=previous_;
}
} // namespace tetra::probes

void* operator new(std::size_t bytes) { return tetra::probes::dc_volume_workspace_allocate(bytes,alignof(std::max_align_t)); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes,std::align_val_t alignment) { return tetra::probes::dc_volume_workspace_allocate(bytes,static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t bytes,std::align_val_t alignment) { return ::operator new(bytes,alignment); }
void* operator new(std::size_t bytes,const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch(...) { return nullptr; }
}
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {
  try { return ::operator new[](bytes); } catch(...) { return nullptr; }
}
void* operator new(std::size_t bytes,std::align_val_t alignment,const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes,alignment); } catch(...) { return nullptr; }
}
void* operator new[](std::size_t bytes,std::align_val_t alignment,const std::nothrow_t&) noexcept {
  try { return ::operator new[](bytes,alignment); } catch(...) { return nullptr; }
}
void operator delete(void* pointer) noexcept { tetra::probes::dc_volume_workspace_deallocate(pointer); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer,std::size_t) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer,std::align_val_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer,std::align_val_t) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer,const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer,const std::nothrow_t&) noexcept { ::operator delete[](pointer); }
void operator delete(void* pointer,std::align_val_t,const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer,std::align_val_t,const std::nothrow_t&) noexcept { ::operator delete[](pointer); }
