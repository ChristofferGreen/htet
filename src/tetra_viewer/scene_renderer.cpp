#include "scene_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace tetra_viewer {
namespace {

std::vector<char> read_file(const char* path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file) throw std::runtime_error("unable to load Vulkan shader");
  const auto size = static_cast<std::size_t>(file.tellg());
  std::vector<char> bytes(size);
  file.seekg(0);
  file.read(bytes.data(), static_cast<std::streamsize>(size));
  return bytes;
}

VkShaderModule shader_module(VkDevice device, const char* path) {
  const auto code = read_file(path);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size();
  info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
  VkShaderModule module{};
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) throw std::runtime_error("unable to create Vulkan shader module");
  return module;
}

std::uint32_t memory_type(VkPhysicalDevice physical_device, std::uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
  for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    if ((bits & (1U << index)) != 0U && (properties.memoryTypes[index].propertyFlags & flags) == flags) return index;
  throw std::runtime_error("no suitable Vulkan memory type");
}

}  // namespace

void SceneRenderer::initialize(VkPhysicalDevice physical_device, VkDevice device, VkFormat colour_format, VkFormat depth_format) {
  physical_device_ = physical_device;
  device_ = device;
  colour_format_ = colour_format;
  depth_format_ = depth_format;

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 28};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS) throw std::runtime_error("unable to create scene pipeline layout");

  const auto vertex_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/scene.vert.spv");
  const auto fragment_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/scene.frag.spv");
  const auto wire_fragment_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/wire.frag.spv");
  const auto edge_vertex_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/edge.vert.spv");
  const auto edge_fragment_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/edge.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main"};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, "main"};
  VkPipelineShaderStageCreateInfo wire_stages[2]{stages[0], stages[1]};
  wire_stages[1].module = wire_fragment_shader;
  VkPipelineShaderStageCreateInfo edge_stages[2]{};
  edge_stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, edge_vertex_shader, "main"};
  edge_stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, edge_fragment_shader, "main"};
  VkVertexInputBindingDescription binding{0, sizeof(SceneVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[6]{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24}, {3, 0, VK_FORMAT_R32G32_SFLOAT, 36}, {4, 0, VK_FORMAT_R32G32B32_SFLOAT, 48}, {5, 0, VK_FORMAT_R32_SFLOAT, 44}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1; vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 6; vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = 1; viewport.scissorCount = 1;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend_attachment{}; blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT; blend_attachment.blendEnable = VK_FALSE;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &colour_format_; rendering.depthAttachmentFormat = depth_format_;
  const auto create_pipeline = [&](const VkPipelineShaderStageCreateInfo* pipeline_stages,
                                   VkPolygonMode polygon_mode, bool depth_test,
                                   bool depth_overlay,
                                   bool alpha_blend, bool offset_fill,
                                   VkPipeline* target) {
    constexpr VkPrimitiveTopology topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = topology;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = polygon_mode;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    // This is the conventional hidden-line setup: move the filled polygons
    // away by the rasterizer's minimum depth units, then draw their unbiased
    // native edges. It avoids coplanar fill/line fighting without making rear
    // edges visible through genuinely nearer geometry.
    raster.depthBiasEnable = offset_fill ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = offset_fill ? 1.0F : 0.0F;
    raster.depthBiasSlopeFactor = offset_fill ? 1.0F : 0.0F;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = depth_overlay ? VK_FALSE : VK_TRUE;
    depth.depthCompareOp = depth_overlay ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState pipeline_blend_attachment=blend_attachment;
    if(alpha_blend){
      pipeline_blend_attachment.blendEnable=VK_TRUE;
      pipeline_blend_attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
      pipeline_blend_attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      pipeline_blend_attachment.colorBlendOp=VK_BLEND_OP_ADD;
      pipeline_blend_attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
      pipeline_blend_attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
      pipeline_blend_attachment.alphaBlendOp=VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo pipeline_blend=blend;
    pipeline_blend.pAttachments=&pipeline_blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering; pipeline.stageCount = 2; pipeline.pStages = pipeline_stages; pipeline.pVertexInputState = &vertex_input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &pipeline_blend; pipeline.pDynamicState = &dynamic; pipeline.layout = pipeline_layout_;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, target) != VK_SUCCESS) throw std::runtime_error("unable to create scene pipeline");
  };
  create_pipeline(stages, VK_POLYGON_MODE_FILL, true, false, false, true, &triangle_pipeline_);
  // The surface wire pass rasterizes the exact same triangle vertices as the
  // opaque pass. Each edge is therefore a native one-pixel line between its
  // two endpoints, with identical depth interpolation and no depth pull.
  create_pipeline(wire_stages, VK_POLYGON_MODE_LINE, true, true, false, false, &triangle_wire_pipeline_);
  // Hierarchy edges are independent segments, expanded into screen-space
  // ribbons by edge.vert. They retain alpha coverage for antialiasing.
  create_pipeline(edge_stages, VK_POLYGON_MODE_FILL, true, true, true, false, &line_pipeline_);
  // Editor objects and manipulation handles remain visible when they overlap
  // the inspected mesh, as in conventional DCC applications. They use their
  // own buffer and pipeline so mesh-edge visibility remains depth correct.
  create_pipeline(edge_stages, VK_POLYGON_MODE_FILL, false, true, true, false,
                  &editor_line_pipeline_);
  vkDestroyShaderModule(device_, vertex_shader, nullptr); vkDestroyShaderModule(device_, fragment_shader, nullptr);
  vkDestroyShaderModule(device_, wire_fragment_shader, nullptr);
  vkDestroyShaderModule(device_, edge_vertex_shader, nullptr); vkDestroyShaderModule(device_, edge_fragment_shader, nullptr);
}

void SceneRenderer::recreate(VkExtent2D extent, std::uint32_t image_count) {
  for (auto& depth : depth_images_) { vkDestroyImageView(device_, depth.view, nullptr); vkDestroyImage(device_, depth.image, nullptr); vkFreeMemory(device_, depth.memory, nullptr); }
  depth_images_.assign(image_count, {});
  for (auto& depth : depth_images_) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = depth_format_; image.extent = {extent.width, extent.height, 1}; image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vkCreateImage(device_, &image, nullptr, &depth.image);
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device_, depth.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memory_type(physical_device_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &allocation, nullptr, &depth.memory); vkBindImageMemory(device_, depth.image, depth.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = depth.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = depth_format_; view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; view.subresourceRange.levelCount = 1; view.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view, nullptr, &depth.view);
  }
}

void SceneRenderer::upload(std::span<const SceneVertex> triangle_vertices,
                           std::span<const SceneVertex> hierarchy_line_vertices,
                           std::span<const SceneVertex> surface_line_vertices) {
  const auto upload_buffer = [this](VertexBuffer& destination, std::span<const SceneVertex> vertices) {
    destination.count = vertices.size();
    if (vertices.size() > destination.capacity) {
      vkDestroyBuffer(device_, destination.buffer, nullptr); vkFreeMemory(device_, destination.memory, nullptr);
      destination.capacity = std::max<std::size_t>(vertices.size(), 4096);
      VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size = destination.capacity * sizeof(SceneVertex); buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      if (vkCreateBuffer(device_, &buffer, nullptr, &destination.buffer) != VK_SUCCESS) throw std::runtime_error("unable to create scene vertex buffer");
      VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device_, destination.buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memory_type(physical_device_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (vkAllocateMemory(device_, &allocation, nullptr, &destination.memory) != VK_SUCCESS) throw std::runtime_error("unable to allocate scene vertex buffer");
      vkBindBufferMemory(device_, destination.buffer, destination.memory, 0);
    }
    if (!vertices.empty()) { void* mapped{}; vkMapMemory(device_, destination.memory, 0, vertices.size_bytes(), 0, &mapped); std::memcpy(mapped, vertices.data(), vertices.size_bytes()); vkUnmapMemory(device_, destination.memory); }
  };
  std::vector<SceneVertex> hierarchy_ribbons;
  std::vector<SceneVertex> editor_ribbons;
  expand_line_segments_for_upload(hierarchy_line_vertices,hierarchy_ribbons);
  expand_line_segments_for_upload(surface_line_vertices,editor_ribbons);
  upload_buffer(triangles_, triangle_vertices);
  upload_buffer(hierarchy_lines_, hierarchy_ribbons);
  upload_buffer(editor_lines_, editor_ribbons);
  surface_upload_planner_.reset();
}

void SceneRenderer::upload_editor_lines(
    std::span<const SceneVertex> editor_line_vertices) {
  std::vector<SceneVertex> ribbons;
  expand_line_segments_for_upload(editor_line_vertices,ribbons);
  editor_lines_.count=ribbons.size();
  if(ribbons.size()>editor_lines_.capacity){
    if(editor_lines_.buffer!=VK_NULL_HANDLE)
      vkDestroyBuffer(device_,editor_lines_.buffer,nullptr);
    if(editor_lines_.memory!=VK_NULL_HANDLE)
      vkFreeMemory(device_,editor_lines_.memory,nullptr);
    editor_lines_.capacity=std::max<std::size_t>(ribbons.size(),4096U);
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=editor_lines_.capacity*sizeof(SceneVertex);
    buffer.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if(vkCreateBuffer(device_,&buffer,nullptr,&editor_lines_.buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create editor line buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_,editor_lines_.buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&editor_lines_.memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate editor line buffer");
    vkBindBufferMemory(device_,editor_lines_.buffer,editor_lines_.memory,0);
  }
  if(!ribbons.empty()){
    void* mapped{};
    vkMapMemory(device_,editor_lines_.memory,0,ribbons.size()*sizeof(SceneVertex),0,&mapped);
    std::memcpy(mapped,ribbons.data(),ribbons.size()*sizeof(SceneVertex));
    vkUnmapMemory(device_,editor_lines_.memory);
  }
}

void SceneRenderer::upload_surface_ranges(
    const SurfaceHostStagingStorage& surface,
    std::span<const SceneVertex> hierarchy_line_vertices,
    std::span<const SceneVertex> editor_line_vertices) {
  surface_upload_planner_.prepare(surface,triangles_.capacity);
  VertexBuffer replacement;
  const auto destroy_buffer=[this](VertexBuffer& buffer){
    if(buffer.buffer!=VK_NULL_HANDLE)vkDestroyBuffer(device_,buffer.buffer,nullptr);
    if(buffer.memory!=VK_NULL_HANDLE)vkFreeMemory(device_,buffer.memory,nullptr);
    buffer={};
  };
  const auto allocate_buffer=[this,&destroy_buffer](std::size_t capacity){
    VertexBuffer buffer;
    buffer.capacity=capacity;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size=capacity*sizeof(SceneVertex);
    info.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if(vkCreateBuffer(device_,&info,nullptr,&buffer.buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create retained surface vertex buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_,buffer.buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&buffer.memory)!=VK_SUCCESS){
      vkDestroyBuffer(device_,buffer.buffer,nullptr);
      throw std::runtime_error("unable to allocate retained surface vertex buffer");
    }
    if(vkBindBufferMemory(device_,buffer.buffer,buffer.memory,0)!=VK_SUCCESS){
      destroy_buffer(buffer);
      throw std::runtime_error("unable to bind retained surface vertex buffer");
    }
    return buffer;
  };
  const auto copy_uploads=[this,&surface](VertexBuffer& destination){
    if(surface_upload_planner_.uploads().empty())return;
    void* mapped{};
    if(vkMapMemory(device_,destination.memory,0,
                   destination.capacity*sizeof(SceneVertex),0,&mapped)!=VK_SUCCESS)
      throw std::runtime_error("unable to map retained surface vertex buffer");
    auto* vertices=static_cast<SceneVertex*>(mapped);
    for(const auto upload:surface_upload_planner_.uploads())
      std::memcpy(vertices+upload.destination_vertex_begin,
                  surface.arena().data()+upload.source_vertex_begin,
                  upload.vertex_count*sizeof(SceneVertex));
    vkUnmapMemory(device_,destination.memory);
  };
  try{
    if(surface_upload_planner_.metrics().full_reallocation){
      const auto required=surface_upload_planner_.metrics().required_vertex_capacity;
      const auto grown=std::max({required,triangles_.capacity*2U,
                                 std::size_t{4096U}});
      replacement=allocate_buffer(grown);
      copy_uploads(replacement);
      if(vkDeviceWaitIdle(device_)!=VK_SUCCESS)
        throw std::runtime_error("unable to idle Vulkan before surface publication");
      std::swap(triangles_,replacement);
      destroy_buffer(replacement);
    }else if(!surface_upload_planner_.uploads().empty()){
      if(vkDeviceWaitIdle(device_)!=VK_SUCCESS)
        throw std::runtime_error("unable to idle Vulkan before partial surface upload");
      copy_uploads(triangles_);
    }
    triangles_.count=0U;
    for(const auto draw:surface_upload_planner_.candidate_draws())
      triangles_.count+=draw.vertex_count;
    surface_upload_planner_.commit();
  }catch(...){
    destroy_buffer(replacement);
    surface_upload_planner_.cancel();
    throw;
  }

  const auto upload_lines=[this,&destroy_buffer](
      VertexBuffer& destination,std::span<const SceneVertex> vertices){
    destination.count=vertices.size();
    if(vertices.size()>destination.capacity){
      destroy_buffer(destination);
      destination.capacity=std::max<std::size_t>(vertices.size(),4096U);
      VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      buffer.size=destination.capacity*sizeof(SceneVertex);
      buffer.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      if(vkCreateBuffer(device_,&buffer,nullptr,&destination.buffer)!=VK_SUCCESS)
        throw std::runtime_error("unable to create scene line buffer");
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(device_,destination.buffer,&requirements);
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize=requirements.size;
      allocation.memoryTypeIndex=memory_type(
          physical_device_,requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if(vkAllocateMemory(device_,&allocation,nullptr,&destination.memory)!=VK_SUCCESS)
        throw std::runtime_error("unable to allocate scene line buffer");
      vkBindBufferMemory(device_,destination.buffer,destination.memory,0);
    }
    if(!vertices.empty()){
      void* mapped{};
      vkMapMemory(device_,destination.memory,0,vertices.size_bytes(),0,&mapped);
      std::memcpy(mapped,vertices.data(),vertices.size_bytes());
      vkUnmapMemory(device_,destination.memory);
    }
  };
  std::vector<SceneVertex> hierarchy_ribbons,editor_ribbons;
  expand_line_segments_for_upload(hierarchy_line_vertices,hierarchy_ribbons);
  expand_line_segments_for_upload(editor_line_vertices,editor_ribbons);
  upload_lines(hierarchy_lines_,hierarchy_ribbons);
  upload_lines(editor_lines_,editor_ribbons);
}

void SceneRenderer::record(VkCommandBuffer command_buffer, VkImageView colour_view, std::uint32_t image_index, VkExtent2D extent, const float* camera_data) const {
  VkClearValue colour{}; colour.color = {{0.06F, 0.08F, 0.11F, 1.0F}};
  VkClearValue depth{}; depth.depthStencil = {1.0F, 0};
  VkRenderingAttachmentInfo colour_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; colour_attachment.imageView = colour_view; colour_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; colour_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colour_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; colour_attachment.clearValue = colour;
  VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth_attachment.imageView = depth_images_.at(image_index).view; depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; depth_attachment.clearValue = depth;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = extent; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &colour_attachment; rendering.pDepthAttachment = &depth_attachment;
  const auto begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(device_, "vkCmdBeginRenderingKHR"));
  const auto end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(device_, "vkCmdEndRenderingKHR"));
  if (begin_rendering == nullptr || end_rendering == nullptr) throw std::runtime_error("dynamic rendering is unavailable");
  begin_rendering(command_buffer, reinterpret_cast<const VkRenderingInfoKHR*>(&rendering));
  VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F}; VkRect2D scissor{{0, 0}, extent}; VkDeviceSize offset{};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport); vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  std::array<float,28> push_data{};
  std::copy_n(camera_data,24,push_data.begin());
  push_data[24]=static_cast<float>(extent.width);
  push_data[25]=static_cast<float>(extent.height);
  // One-pixel half-extent provides the complete filter footprint for an
  // analytically antialiased one-pixel centre line.
  push_data[26]=1.0F;
  const auto draw = [&](VkPipeline pipeline, const VertexBuffer& vertices,
                        std::span<const SurfaceDeviceDrawRange> ranges={}) {
    if (vertices.count == 0) return;
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertices.buffer, &offset);
    vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 28, push_data.data());
    if(ranges.empty())
      vkCmdDraw(command_buffer,static_cast<std::uint32_t>(vertices.count),1,0,0);
    else for(const auto range:ranges)
      vkCmdDraw(command_buffer,static_cast<std::uint32_t>(range.vertex_count),1,
                static_cast<std::uint32_t>(range.first_vertex),0);
  };
  // Opaque geometry establishes visibility first. The native line-mode pass
  // reuses those triangle vertices and depths, so hidden rear edges fail the
  // depth test and visible edges do not depend on triangle shape.
  draw(triangle_pipeline_,triangles_,surface_upload_planner_.published_draws());
  draw(triangle_wire_pipeline_,triangles_,surface_upload_planner_.published_draws());
  draw(line_pipeline_, hierarchy_lines_);
  draw(editor_line_pipeline_, editor_lines_);
  end_rendering(command_buffer);
}

void SceneRenderer::shutdown() {
  for (auto& depth : depth_images_) { vkDestroyImageView(device_, depth.view, nullptr); vkDestroyImage(device_, depth.image, nullptr); vkFreeMemory(device_, depth.memory, nullptr); }
  for (const VertexBuffer& buffer : {triangles_, hierarchy_lines_, editor_lines_}) { vkDestroyBuffer(device_, buffer.buffer, nullptr); vkFreeMemory(device_, buffer.memory, nullptr); }
  vkDestroyPipeline(device_, triangle_pipeline_, nullptr);
  vkDestroyPipeline(device_, triangle_wire_pipeline_, nullptr);
  vkDestroyPipeline(device_, line_pipeline_, nullptr);
  vkDestroyPipeline(device_, editor_line_pipeline_, nullptr);
  vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
}

}  // namespace tetra_viewer
