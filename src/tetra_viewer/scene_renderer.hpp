#pragma once

#include "tetra_viewer/atmosphere.hpp"
#include "tetra_viewer/shadow_cascades.hpp"
#include "tetra_viewer/viewer_scene.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tetra_viewer {

struct AtmosphereFrameInput {
  AtmosphereParameters parameters{};
  tetra::Vec3 planet_centre_relative_world{};
  tetra::Vec3 camera_relative_world{};
  tetra::Vec3 camera_right{-1.0,0.0,0.0};
  tetra::Vec3 camera_down{0.0,-1.0,0.0};
  tetra::Vec3 camera_forward{0.0,0.0,-1.0};
  tetra::Vec3 sun_direction{0.0,1.0,0.0};
  double vertical_tangent{1.0};
  double aspect_ratio{1.0};
  double maximum_aerial_distance_metres{1'000'000.0};
  float exposure{0.65F};
  int debug_view{};
  AtmosphereQuality quality{AtmosphereQuality::standard};
  bool enabled{};
};

struct SceneGpuTimings {
  double shadows_milliseconds{};
  double atmosphere_milliseconds{};
  double terrain_milliseconds{};
  double composite_milliseconds{};
  bool valid{};
};

class SceneRenderer {
 public:
  void initialize(VkPhysicalDevice physical_device, VkDevice device, VkFormat colour_format, VkFormat depth_format);
  void recreate(VkExtent2D extent, std::uint32_t image_count,
                AtmosphereQuality quality=AtmosphereQuality::standard);
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
  [[nodiscard]] const SceneGpuTimings& gpu_timings() const noexcept {
    return gpu_timings_;
  }
  [[nodiscard]] std::size_t atmosphere_allocation_bytes() const noexcept {
    return atmosphere_allocation_bytes_;
  }
  [[nodiscard]] std::size_t scene_target_allocation_bytes() const noexcept {
    return scene_target_allocation_bytes_;
  }
  // camera_data is a column-major view-projection matrix followed by the
  // legacy diagnostic light, rendering parameters, and relative view point.
  void record(VkCommandBuffer command_buffer, VkImageView colour_view,
              std::uint32_t image_index,VkExtent2D extent,
              const float* camera_data,const AtmosphereFrameInput& atmosphere,
              bool black_clear=false);
  void shutdown();

 private:
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkFormat colour_format_{VK_FORMAT_UNDEFINED};
  VkFormat depth_format_{VK_FORMAT_UNDEFINED};
  VkFormat scene_colour_format_{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkDescriptorSetLayout descriptor_set_layout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout composite_descriptor_set_layout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout atmosphere_descriptor_set_layout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
  VkSampler shadow_sampler_{VK_NULL_HANDLE};
  VkSampler scene_sampler_{VK_NULL_HANDLE};
  VkSampler depth_sampler_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout shaded_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout atmosphere_pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline shadow_pipeline_{VK_NULL_HANDLE};
  VkPipeline sky_pipeline_{VK_NULL_HANDLE};
  VkPipeline composite_pipeline_{VK_NULL_HANDLE};
  VkPipeline atmosphere_pipeline_{VK_NULL_HANDLE};
  VkPipeline triangle_pipeline_{VK_NULL_HANDLE};
  VkPipeline triangle_wire_pipeline_{VK_NULL_HANDLE};
  VkPipeline line_pipeline_{VK_NULL_HANDLE};
  VkPipeline editor_line_pipeline_{VK_NULL_HANDLE};
  VkQueryPool timing_query_pool_{VK_NULL_HANDLE};
  float timestamp_period_nanoseconds_{};
  std::vector<bool> timing_queries_written_;
  SceneGpuTimings gpu_timings_{};
  std::size_t atmosphere_allocation_bytes_{};
  std::size_t scene_target_allocation_bytes_{};
  AtmosphereQualitySettings quality_settings_{
      atmosphere_quality_settings(AtmosphereQuality::standard)};
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
  struct DepthImage { VkImage image{VK_NULL_HANDLE}; VkDeviceMemory memory{VK_NULL_HANDLE}; VkImageView view{VK_NULL_HANDLE}; bool initialized{}; };
  std::vector<DepthImage> depth_images_;
  struct SceneColourImage { VkImage image{VK_NULL_HANDLE}; VkDeviceMemory memory{VK_NULL_HANDLE}; VkImageView view{VK_NULL_HANDLE}; bool initialized{}; };
  std::vector<SceneColourImage> scene_colour_images_;
  struct ShadowImage : DepthImage {
    std::array<VkImageView,shadow_cascade_count> layer_views{};
    VkBuffer uniform_buffer{VK_NULL_HANDLE};
    VkDeviceMemory uniform_memory{VK_NULL_HANDLE};
  };
  std::vector<ShadowImage> shadow_images_;
  struct AtmosphereImage {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
  };
  struct AtmosphereFrameResources {
    AtmosphereImage transmittance;
    AtmosphereImage multiple_scattering;
    AtmosphereImage sky_view;
    AtmosphereImage aerial_scattering;
    AtmosphereImage aerial_transmittance;
    VkBuffer uniform_buffer{VK_NULL_HANDLE};
    VkDeviceMemory uniform_memory{VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
    std::uint64_t optical_hash{};
    bool images_initialized{};
  };
  std::vector<AtmosphereFrameResources> atmosphere_frames_;
  std::vector<VkDescriptorSet> descriptor_sets_;
  std::vector<VkDescriptorSet> composite_descriptor_sets_;
};

}  // namespace tetra_viewer
