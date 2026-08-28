#pragma once

#include "tetra_viewer/viewer_scene.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tetra_viewer {

class SceneRenderer {
 public:
  void initialize(VkPhysicalDevice physical_device, VkDevice device, VkFormat colour_format, VkFormat depth_format);
  void recreate(VkExtent2D extent, std::uint32_t image_count);
  void upload(std::span<const SceneVertex> triangle_vertices,
              std::span<const SceneVertex> hierarchy_line_vertices,
              std::span<const SceneVertex> surface_line_vertices);
  void upload_surface_ranges(
      const SurfaceHostStagingStorage& surface,
      std::span<const SceneVertex> hierarchy_line_vertices,
      std::span<const SceneVertex> editor_line_vertices);
  // Refresh view-dependent camera/manipulator overlays without touching the
  // substantially larger retained mesh buffers.
  void upload_editor_lines(std::span<const SceneVertex> editor_line_vertices);
  [[nodiscard]] const SurfaceDeviceUploadMetrics& surface_upload_metrics()
      const noexcept { return surface_upload_planner_.metrics(); }
  // camera_data is a column-major view-projection matrix followed by a
  // camera-relative key-light direction and rendering-mode parameters.
  void record(VkCommandBuffer command_buffer, VkImageView colour_view,
              std::uint32_t image_index,VkExtent2D extent,
              const float* camera_data,bool black_clear=false) const;
  void shutdown();

 private:
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkFormat colour_format_{VK_FORMAT_UNDEFINED};
  VkFormat depth_format_{VK_FORMAT_UNDEFINED};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline triangle_pipeline_{VK_NULL_HANDLE};
  VkPipeline triangle_wire_pipeline_{VK_NULL_HANDLE};
  VkPipeline line_pipeline_{VK_NULL_HANDLE};
  VkPipeline editor_line_pipeline_{VK_NULL_HANDLE};
  struct VertexBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    std::size_t capacity{};
    std::size_t count{};
  };
  VertexBuffer triangles_;
  VertexBuffer hierarchy_lines_;
  VertexBuffer editor_lines_;
  SurfaceDeviceUploadPlanner surface_upload_planner_;
  struct DepthImage { VkImage image{VK_NULL_HANDLE}; VkDeviceMemory memory{VK_NULL_HANDLE}; VkImageView view{VK_NULL_HANDLE}; };
  std::vector<DepthImage> depth_images_;
};

}  // namespace tetra_viewer
