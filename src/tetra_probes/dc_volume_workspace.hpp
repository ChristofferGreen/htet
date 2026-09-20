#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace tetra::probes {

// A caller-owned backing store for one generic DC-volume publication.  While
// active, all ordinary C++ allocations on the calling thread are carved from
// this block; exhaustion throws std::bad_alloc rather than falling back to the
// heap.  Published vectors retain pointers into the workspace, so reset() is
// legal only after the corresponding result has been destroyed.
class DcVolumeBuildWorkspace {
 public:
  explicit DcVolumeBuildWorkspace(std::size_t bytes);
  ~DcVolumeBuildWorkspace();
  DcVolumeBuildWorkspace(const DcVolumeBuildWorkspace&)=delete;
  DcVolumeBuildWorkspace& operator=(const DcVolumeBuildWorkspace&)=delete;

  [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t used_bytes() const noexcept { return used_; }
  [[nodiscard]] std::size_t allocation_count() const noexcept { return allocations_; }
  [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }

  // Invalidates every result previously built in this workspace.
  void reset() noexcept;
  [[nodiscard]] bool owns(const void* pointer) const noexcept;
  [[nodiscard]] DcVolumeBuildWorkspace* registry_next() const noexcept { return registry_next_; }
  // Used exclusively by the scoped global-allocation route below.
  [[nodiscard]] void* allocate(std::size_t bytes,std::size_t alignment);
  void deallocate(void* pointer) noexcept;

 private:
  friend class DcVolumeBuildWorkspaceScope;
  std::byte* storage_{};
  std::size_t capacity_{};
  std::size_t used_{};
  std::size_t live_bytes_{};
  std::size_t allocations_{};
  bool exhausted_{};
  bool active_{};
  struct Block;
  static constexpr std::size_t maximum_orders_{64U};
  std::array<Block*,maximum_orders_> free_lists_{};
  std::size_t maximum_order_{};
  DcVolumeBuildWorkspace* registry_next_{};
};

// Global allocation interposition calls these only while a workspace scope is
// active. They are intentionally not part of the volume construction API.
[[nodiscard]] void* dc_volume_workspace_allocate(std::size_t bytes,std::size_t alignment);
[[nodiscard]] bool dc_volume_workspace_owns(const void* pointer) noexcept;
void dc_volume_workspace_deallocate(void* pointer) noexcept;

class DcVolumeBuildWorkspaceScope {
 public:
  explicit DcVolumeBuildWorkspaceScope(DcVolumeBuildWorkspace& workspace) noexcept;
  ~DcVolumeBuildWorkspaceScope();
  [[nodiscard]] bool active() const noexcept { return active_; }
 private:
  DcVolumeBuildWorkspace* previous_{};
  DcVolumeBuildWorkspace* workspace_{};
  bool active_{};
};

} // namespace tetra::probes
