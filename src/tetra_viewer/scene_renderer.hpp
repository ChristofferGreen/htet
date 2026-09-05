#pragma once

#include "tetra_viewer/atmosphere.hpp"
#include "tetra_viewer/atmosphere_shadow_front.hpp"
#include "tetra_viewer/shadow_cascades.hpp"
#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_core/gpu_hierarchy_snapshot.hpp"

#include <vulkan/vulkan.h>
#include <array>

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
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
  double minimum_analytic_ground_distance_metres{5'000.0};
  float exposure{0.65F};
  int debug_view{};
  AtmosphereQuality quality{AtmosphereQuality::standard};
  AtmosphereTransport transport{default_atmosphere_transport};
  AtmosphereRenderingMethod rendering_method{
      default_atmosphere_rendering_method};
  std::uint32_t screen_resolution_divisor{4U};
  AtmosphereShadowIntegrator shadow_integrator{
      default_atmosphere_shadow_integrator};
  SurfaceShadowBiasMode surface_shadow_bias{SurfaceShadowBiasMode::slope_scaled};
  AtmosphereShadowFilter shadow_filter{default_atmosphere_shadow_filter};
  float shadow_raster_bias_constant{0.8125F};
  float shadow_raster_bias_slope{1.21875F};
  double atmosphere_shadow_comparison_bias_world_override{-1.0};
  const AtmosphereShadowFront* shadow_front{};
  bool numeric_probe_requested{};
  bool shadow_projection_probe_requested{};
  bool capture_requested{};
  bool dynamic_sun{};
  bool enabled{};
};

struct AtmosphereGpuProbe {
  std::array<std::array<float,4>,atmosphere_numeric_probe_value_count> values{};
  bool valid{};
};

struct SceneCapture {
  std::vector<std::uint8_t> pixels;
  std::vector<float> reversed_depth;
  std::uint32_t width{};
  std::uint32_t height{};
  bool bgra{};
  bool valid{};
};

struct SceneGpuTimings {
  double shadows_milliseconds{};
  double atmosphere_milliseconds{};
  double terrain_milliseconds{};
  double depth_reduction_milliseconds{};
  double screen_integration_milliseconds{};
  double temporal_reconstruction_milliseconds{};
  double composite_milliseconds{};
  bool valid{};
};

// State produced by the diagnostic-only Vulkan hierarchy selector.  The CPU
// retained surface remains the renderer's authority until a later parity gate.
struct GpuLodDispatchStatus {
  std::uint64_t source_revision{};
  std::uint32_t hierarchy_records{};
  std::uint32_t selected_records{};
  bool dispatched{};
  bool overflow{};
};
struct GpuTerrainExtractStatus {
  std::uint64_t source_revision{};
  std::uint32_t cells{};
  std::uint32_t vertices{};
  std::uint32_t expected_vertices{};
  std::uint32_t linear_expected_vertices{};
  std::uint32_t shader_linear_vertices{};
  double milliseconds{};
  bool complete{};
  bool input_overflow{};
  bool overflow{};
  bool vertex_count_matches_cpu{};
};

struct AtmosphereDispatchCounts {
  std::uint64_t transmittance{};
  std::uint64_t multiple_scattering{};
  std::uint64_t sky_view{};
  std::uint64_t sky_irradiance{};
  std::uint64_t aerial_perspective{};
  std::uint64_t long_shadow{};
  std::uint64_t screen_reconstruction{};
  std::uint64_t temporal_history_accepts{};
  std::uint64_t temporal_history_invalidations{};
  std::uint64_t optical_changes{};
  std::uint64_t scattering_changes{};
  std::uint64_t sun_changes{};
  std::uint64_t camera_position_changes{};
  std::uint64_t sky_position_changes{};
  std::uint64_t camera_orientation_changes{};
  std::uint64_t shadow_integrator_changes{};
  std::uint64_t shadow_changes{};
  std::uint64_t render_origin_changes{};
};

struct AtmosphereShadowMapStatus {
  std::uint64_t revision{};
  std::uint64_t refreshes{};
  std::uint64_t local_cascade_refreshes{};
  std::uint64_t depth_generation{};
  std::uint64_t hierarchy_generation{};
  std::size_t caster_draws{};
  std::array<double,shadow_cascade_count> local_depth_world_spans{};
  std::array<double,shadow_cascade_count> local_texel_world_sizes{};
  std::array<double,shadow_cascade_count> local_comparison_biases_normalized{};
  std::array<double,shadow_cascade_count> local_comparison_biases_world{};
  double fitted_depth_world_span{};
  double fitted_texel_world_size_x{};
  double fitted_texel_world_size_y{};
  double comparison_bias_normalized{};
  double comparison_bias_world{};
  double raster_bias_constant{0.8125};
  double raster_bias_slope{1.21875};
  std::size_t epipolar_radial_resolution{};
  std::size_t epipolar_angular_rows{};
  std::size_t epipolar_elements{};
  std::uint64_t epipolar_visited_nodes{};
  std::uint64_t epipolar_emitted_intervals{};
  std::uint64_t epipolar_fallbacks{};
  std::uint64_t epipolar_overflows{};
  std::uint64_t epipolar_hierarchy_refreshes{};
  AtmosphereShadowIntegrator integrator{default_atmosphere_shadow_integrator};
  bool hierarchy_complete{};
  bool integration_fallback{};
  bool complete{};
};

class SceneRenderer {
 public:
  void initialize(VkPhysicalDevice physical_device, VkDevice device, VkFormat colour_format, VkFormat depth_format);
  void recreate(VkExtent2D extent, std::uint32_t image_count,
                AtmosphereQuality quality=AtmosphereQuality::standard,
                std::uint32_t screen_resolution_divisor=4U,
                VkSampleCountFlagBits terrain_samples=VK_SAMPLE_COUNT_1_BIT);
  void configure_terrain_msaa(VkSampleCountFlagBits terrain_samples);
  void set_render_extent(VkExtent2D extent,float sharpening);
  [[nodiscard]] bool supports_terrain_msaa() const noexcept {
    return supports_terrain_samples(VK_SAMPLE_COUNT_2_BIT)||
           supports_terrain_samples(VK_SAMPLE_COUNT_4_BIT);
  }
  [[nodiscard]] bool supports_terrain_samples(
      VkSampleCountFlagBits samples) const noexcept {
    return samples==VK_SAMPLE_COUNT_1_BIT||
           (terrain_sample_counts_&samples)!=0U;
  }
  [[nodiscard]] bool terrain_msaa_memoryless() const noexcept {
    return terrain_msaa_memoryless_;
  }
  [[nodiscard]] bool prefers_tile_local_terrain_msaa() const noexcept {
    return terrain_msaa_memoryless_supported_&&
           supports_terrain_samples(VK_SAMPLE_COUNT_4_BIT);
  }
  [[nodiscard]] VkExtent2D allocated_extent() const noexcept {
    return allocated_extent_;
  }
  [[nodiscard]] VkExtent2D render_extent() const noexcept {
    return render_extent_;
  }
  void upload(std::span<const SceneVertex> triangle_vertices,
              std::span<const SceneVertex> hierarchy_line_vertices,
              std::span<const SceneVertex> surface_line_vertices);
  void upload_surface_ranges(
      const SurfaceHostStagingStorage& surface,
      std::span<const SceneVertex> hierarchy_line_vertices,
      std::span<const SceneVertex> editor_line_vertices);
  void upload_gpu_hierarchy_snapshot(const tetra::GpuHierarchySnapshot& snapshot);
  void stage_gpu_terrain_cells(
      std::span<const tetra::GpuTerrainCellRecord> cells,
      std::uint64_t source_revision,std::uint32_t expected_vertices);
  void set_gpu_lod_diagnostic_enabled(bool enabled) noexcept {
    gpu_lod_diagnostic_enabled_=enabled;
  }
  // Refresh view-dependent camera/manipulator overlays without touching the
  // substantially larger retained mesh buffers.
  void upload_editor_lines(std::span<const SceneVertex> editor_line_vertices);
  [[nodiscard]] const SurfaceDeviceUploadMetrics& surface_upload_metrics()
      const noexcept { return surface_upload_planner_.metrics(); }
  [[nodiscard]] const SceneGpuTimings& gpu_timings() const noexcept {
    return gpu_timings_;
  }
  [[nodiscard]] const GpuLodDispatchStatus& gpu_lod_dispatch_status() const
      noexcept { return gpu_lod_dispatch_status_; }
  [[nodiscard]] std::uint64_t gpu_lod_uploaded_revision() const noexcept {
    return gpu_lod_uploaded_revision_;
  }
  [[nodiscard]] std::uint64_t gpu_terrain_cells_revision() const noexcept {
    return gpu_terrain_cells_revision_;
  }
  [[nodiscard]] const GpuTerrainExtractStatus& gpu_terrain_extract_status()
      const noexcept { return gpu_terrain_extract_status_; }
  [[nodiscard]] const SurfaceDrawVisibility& terrain_draw_visibility()
      const noexcept { return terrain_draw_visibility_; }
  [[nodiscard]] const AtmosphereDispatchCounts& atmosphere_dispatch_counts()
      const noexcept { return atmosphere_dispatch_counts_; }
  [[nodiscard]] const AtmosphereShadowMapStatus& atmosphere_shadow_map_status()
      const noexcept { return atmosphere_shadow_map_status_; }
  [[nodiscard]] std::size_t atmosphere_allocation_bytes() const noexcept {
    return atmosphere_allocation_bytes_;
  }
  [[nodiscard]] std::size_t scene_target_allocation_bytes() const noexcept {
    return scene_target_allocation_bytes_;
  }
  [[nodiscard]] const AtmosphereGpuProbe& latest_atmosphere_probe()
      const noexcept { return latest_atmosphere_probe_; }
  [[nodiscard]] const SceneCapture& latest_capture() const noexcept {
    return latest_capture_;
  }
  [[nodiscard]] const std::optional<AtmosphereLookupRevisions>&
  latest_atmosphere_lookup_revisions() const noexcept {
    return atmosphere_lookups_.lookup_revisions;
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
  VkDescriptorSetLayout gpu_lod_descriptor_set_layout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout gpu_terrain_descriptor_set_layout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
  VkSampler shadow_sampler_{VK_NULL_HANDLE};
  VkSampler scene_sampler_{VK_NULL_HANDLE};
  VkSampler depth_sampler_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout shaded_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout atmosphere_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout gpu_lod_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout gpu_terrain_pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline shadow_pipeline_{VK_NULL_HANDLE};
  VkPipeline sky_pipeline_{VK_NULL_HANDLE};
  VkPipeline composite_pipeline_{VK_NULL_HANDLE};
  VkPipeline faithful_composite_pipeline_{VK_NULL_HANDLE};
  VkPipeline atmosphere_pipeline_{VK_NULL_HANDLE};
  VkPipeline faithful_atmosphere_pipeline_{VK_NULL_HANDLE};
  VkPipeline reference_hillaire_atmosphere_pipeline_{VK_NULL_HANDLE};
  VkPipeline gpu_lod_pipeline_{VK_NULL_HANDLE};
  VkPipeline gpu_terrain_extract_pipeline_{VK_NULL_HANDLE};
  VkPipeline triangle_pipeline_{VK_NULL_HANDLE};
  VkPipeline triangle_wire_pipeline_{VK_NULL_HANDLE};
  VkPipeline line_pipeline_{VK_NULL_HANDLE};
  VkPipeline editor_line_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa_sky_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa_triangle_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa_triangle_wire_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa_line_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa_editor_line_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa2_sky_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa2_triangle_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa2_triangle_wire_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa2_line_pipeline_{VK_NULL_HANDLE};
  VkPipeline msaa2_editor_line_pipeline_{VK_NULL_HANDLE};
  VkQueryPool timing_query_pool_{VK_NULL_HANDLE};
  float timestamp_period_nanoseconds_{};
  std::vector<bool> timing_queries_written_;
  SceneGpuTimings gpu_timings_{};
  GpuLodDispatchStatus gpu_lod_dispatch_status_{};
  AtmosphereDispatchCounts atmosphere_dispatch_counts_{};
  std::uint32_t dynamic_sun_shadow_phase_{};
  bool dynamic_sun_was_active_{};
  static constexpr std::uint32_t long_shadow_update_phase_count=16U;
  std::uint32_t long_shadow_update_phase_{long_shadow_update_phase_count};
  bool long_shadow_update_restart_needed_{};
  AtmosphereGpuProbe latest_atmosphere_probe_{};
  AtmosphereShadowMapStatus atmosphere_shadow_map_status_{};
  SceneCapture latest_capture_{};
  std::size_t atmosphere_allocation_bytes_{};
  std::size_t scene_target_allocation_bytes_{};
  std::uint64_t geometry_revision_{};
  AtmosphereQualitySettings quality_settings_{
      atmosphere_quality_settings(AtmosphereQuality::standard)};
  std::uint32_t screen_resolution_divisor_{2U};
  VkSampleCountFlags terrain_sample_counts_{VK_SAMPLE_COUNT_1_BIT};
  VkSampleCountFlagBits terrain_samples_{VK_SAMPLE_COUNT_1_BIT};
  bool terrain_msaa_memoryless_{};
  bool terrain_msaa_memoryless_supported_{};
  VkExtent2D allocated_extent_{};
  VkExtent2D render_extent_{};
  float upscale_sharpening_{};
  struct VertexBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    std::size_t capacity{};
    std::size_t count{};
  };
  VertexBuffer triangles_;
  VertexBuffer hierarchy_lines_;
  VertexBuffer editor_lines_;
  struct GpuLodBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    std::size_t capacity{};
    std::uint64_t revision{};
    std::uint32_t record_count{};
    bool pending{};
  };
  [[nodiscard]] GpuLodBuffer allocate_gpu_lod_buffer(
      std::size_t bytes,VkBufferUsageFlags usage=
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  void destroy_gpu_lod_buffer(GpuLodBuffer& buffer) noexcept;
  void update_gpu_terrain_descriptor_set(std::uint32_t image_index);
  // Called only from record() after the acquired image's fence has completed.
  // Each slot owns its descriptors, so growth cannot invalidate in-flight work
  // on another swapchain image and never needs vkDeviceWaitIdle.
  void ensure_gpu_terrain_slot_capacity(std::uint32_t image_index);
  GpuLodBuffer gpu_lod_hierarchy_;
  std::vector<GpuLodBuffer> gpu_lod_outputs_;
  std::vector<VkDescriptorSet> gpu_lod_descriptor_sets_;
  std::uint64_t gpu_lod_uploaded_revision_{};
  bool gpu_lod_hierarchy_upload_pending_{};
  bool gpu_lod_diagnostic_enabled_{};
  std::vector<tetra::GpuTerrainCellRecord> gpu_terrain_cells_;
  std::uint64_t gpu_terrain_cells_revision_{};
  std::uint64_t gpu_terrain_cells_generation_{};
  std::uint32_t gpu_terrain_expected_vertices_{};
  std::uint32_t gpu_terrain_linear_expected_vertices_{};
  std::size_t gpu_terrain_input_bytes_required_{};
  std::size_t gpu_terrain_output_bytes_required_{};
  std::size_t gpu_terrain_index_bytes_required_{};
  std::vector<GpuLodBuffer> gpu_terrain_cell_buffers_;
  std::vector<GpuLodBuffer> gpu_terrain_output_buffers_;
  std::vector<GpuLodBuffer> gpu_terrain_index_buffers_;
  std::vector<VkDescriptorSet> gpu_terrain_descriptor_sets_;
  std::vector<std::uint64_t> gpu_terrain_slot_revisions_;
  std::vector<bool> gpu_terrain_slot_pending_;
  std::vector<bool> gpu_terrain_timing_pending_;
  std::vector<double> gpu_terrain_slot_milliseconds_;
  GpuTerrainExtractStatus gpu_terrain_extract_status_{};
  SurfaceDeviceUploadPlanner surface_upload_planner_;
  SurfaceDrawVisibility terrain_draw_visibility_{};
  struct DepthImage { VkImage image{VK_NULL_HANDLE}; VkDeviceMemory memory{VK_NULL_HANDLE}; VkImageView view{VK_NULL_HANDLE}; bool initialized{}; };
  std::vector<DepthImage> depth_images_;
  struct SceneColourImage { VkImage image{VK_NULL_HANDLE}; VkDeviceMemory memory{VK_NULL_HANDLE}; VkImageView view{VK_NULL_HANDLE}; bool initialized{}; };
  std::vector<SceneColourImage> scene_colour_images_;
  std::vector<DepthImage> msaa_depth_images_;
  std::vector<SceneColourImage> msaa_colour_images_;
  void destroy_terrain_msaa_targets();
  void create_terrain_msaa_targets(VkExtent2D extent,
                                   std::uint32_t image_count);
  struct ShadowImage : DepthImage {
    std::array<VkImageView,shadow_map_layer_count> layer_views{};
    VkBuffer uniform_buffer{VK_NULL_HANDLE};
    VkDeviceMemory uniform_memory{VK_NULL_HANDLE};
    VkBuffer minmax_buffer{VK_NULL_HANDLE};
    VkDeviceMemory minmax_memory{VK_NULL_HANDLE};
    VkBuffer epipolar_diagnostic_buffer{VK_NULL_HANDLE};
    VkDeviceMemory epipolar_diagnostic_memory{VK_NULL_HANDLE};
    bool epipolar_diagnostic_pending{};
    std::uint64_t atmosphere_shadow_depth_generation{};
    std::uint64_t minmax_generation{};
    std::uint64_t epipolar_generation{};
    std::uint64_t epipolar_source_revision{};
    bool minmax_is_epipolar{};
    std::array<std::array<float,16>,shadow_cascade_count>
        local_shadow_matrices{};
    std::array<std::uint64_t,shadow_cascade_count>
        local_surface_generations{};
    std::array<bool,shadow_cascade_count> local_shadow_initialized{};
    std::array<float,16> atmosphere_shadow_matrix{};
    std::uint64_t atmosphere_surface_generation{};
    std::uint64_t atmosphere_shadow_front_generation{};
    double atmosphere_shadow_receiver_distance{};
    std::uint64_t atmosphere_shadow_pending_generation{};
    bool atmosphere_shadow_pending_completion{};
    bool atmosphere_shadow_pending_complete{};
    bool atmosphere_shadow_initialized{};
  };
  std::vector<ShadowImage> shadow_images_;
  struct AtmosphereImage {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
  };
  struct AtmosphereFrameResources {
    struct TemporalCameraSnapshot {
      tetra::Vec3 position_from_planet_centre_metres{};
      tetra::Vec3 right{-1.0,0.0,0.0};
      tetra::Vec3 down{0.0,-1.0,0.0};
      tetra::Vec3 forward{0.0,0.0,-1.0};
      double tangent_x{1.0};
      double tangent_y{1.0};
    };
    VkBuffer uniform_buffer{VK_NULL_HANDLE};
    VkDeviceMemory uniform_memory{VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
    VkBuffer probe_buffer{VK_NULL_HANDLE};
    VkDeviceMemory probe_memory{VK_NULL_HANDLE};
    AtmosphereImage screen_scattering;
    AtmosphereImage screen_transmittance;
    AtmosphereImage screen_endpoint;
    AtmosphereImage froxel_scattering;
    AtmosphereImage froxel_transmittance;
    std::array<AtmosphereImage,2> history_scattering;
    std::array<AtmosphereImage,2> history_transmittance;
    std::array<AtmosphereImage,2> history_endpoint;
    std::array<AtmosphereImage,2> history_visibility;
    std::array<AtmosphereScreenHistoryIdentity,2> history_identities;
    std::array<TemporalCameraSnapshot,2> history_cameras;
    std::array<std::uint32_t,2> history_sample_counts{};
    std::uint32_t history_sequence{};
    std::uint32_t history_write_index{};
    bool screen_images_initialized{};
    bool history_images_initialized{};
    bool froxel_images_initialized{};
    bool descriptor_images_initialized{};
    bool probe_pending{};
  };
  struct AtmosphereLookupResources {
    AtmosphereImage transmittance;
    AtmosphereImage multiple_scattering;
    AtmosphereImage sky_view;
    AtmosphereImage sky_irradiance;
    AtmosphereImage aerial_scattering;
    AtmosphereImage aerial_transmittance;
    AtmosphereImage long_shadow;
    std::optional<AtmosphereLookupRevisions> lookup_revisions;
    std::optional<AtmosphereLookupSnapshotSet> lookup_snapshots;
    AtmosphereTransport transport{default_atmosphere_transport};
    AtmosphereShadowIntegrator shadow_integrator{
        default_atmosphere_shadow_integrator};
    bool images_initialized{};
  };
  AtmosphereLookupResources atmosphere_lookups_;
  std::vector<AtmosphereFrameResources> atmosphere_frames_;
  struct CaptureFrameResources {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory image_memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory buffer_memory{VK_NULL_HANDLE};
    VkBuffer depth_buffer{VK_NULL_HANDLE};
    VkDeviceMemory depth_buffer_memory{VK_NULL_HANDLE};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    bool initialized{};
    bool pending{};
  };
  void ensure_capture_resources(CaptureFrameResources& capture,
                                VkExtent2D extent);
  std::vector<CaptureFrameResources> capture_frames_;
  std::vector<VkDescriptorSet> descriptor_sets_;
  std::vector<VkDescriptorSet> composite_descriptor_sets_;
};

}  // namespace tetra_viewer
