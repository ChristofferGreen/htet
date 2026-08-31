#include "scene_renderer.hpp"
#include "projection.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace tetra_viewer {
namespace {

void hash_scalar(std::uint64_t& hash,double value) {
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto bits=std::bit_cast<std::uint64_t>(value);
  for(unsigned byte=0;byte<8U;++byte){
    hash^=(bits>>(byte*8U))&0xffU;
    hash*=prime;
  }
}

std::uint64_t hash_vectors(std::initializer_list<tetra::Vec3> vectors,
                           std::initializer_list<double> scalars={}) {
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto vector:vectors){
    hash_scalar(hash,vector.x);
    hash_scalar(hash,vector.y);
    hash_scalar(hash,vector.z);
  }
  for(const double scalar:scalars)hash_scalar(hash,scalar);
  return hash;
}

VkCompareOp depth_compare(DepthConvention convention,bool overlay=false) {
  if(convention==DepthConvention::reversed_infinite)
    return overlay?VK_COMPARE_OP_GREATER_OR_EQUAL:VK_COMPARE_OP_GREATER;
  return overlay?VK_COMPARE_OP_LESS_OR_EQUAL:VK_COMPARE_OP_LESS;
}

std::size_t shadow_minmax_element_count(std::uint32_t resolution) {
  std::size_t count{};
  while(resolution>0U){
    count+=static_cast<std::size_t>(resolution)*resolution;
    if(resolution==1U)break;
    resolution=(resolution+1U)/2U;
  }
  return count;
}

float depth_clear(DepthConvention convention) {
  return convention==DepthConvention::reversed_infinite?0.0F:1.0F;
}

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
  VkPhysicalDeviceProperties device_properties{};
  vkGetPhysicalDeviceProperties(physical_device_,&device_properties);
  timestamp_period_nanoseconds_=device_properties.limits.timestampPeriod;
  VkPhysicalDeviceDepthStencilResolveProperties resolve_properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
  VkPhysicalDeviceProperties2 device_properties_2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  device_properties_2.pNext=&resolve_properties;
  vkGetPhysicalDeviceProperties2(physical_device_,&device_properties_2);
  const auto framebuffer_samples=
      device_properties.limits.framebufferColorSampleCounts&
      device_properties.limits.framebufferDepthSampleCounts;
  const auto supports_four_samples=[&](VkFormat format,
                                        VkImageUsageFlags usage){
    VkImageFormatProperties properties{};
    return vkGetPhysicalDeviceImageFormatProperties(
               physical_device_,format,VK_IMAGE_TYPE_2D,
               VK_IMAGE_TILING_OPTIMAL,usage,0U,&properties)==VK_SUCCESS&&
           (properties.sampleCounts&VK_SAMPLE_COUNT_4_BIT)!=0U;
  };
  terrain_msaa_supported_=
      (framebuffer_samples&VK_SAMPLE_COUNT_4_BIT)!=0U&&
      (resolve_properties.supportedDepthResolveModes&
       VK_RESOLVE_MODE_MAX_BIT)!=0U&&
      supports_four_samples(
          scene_colour_format_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                                   VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)&&
      supports_four_samples(
          depth_format_,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                            VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);

  std::array<VkDescriptorSetLayoutBinding,6> shadow_bindings{};
  shadow_bindings[0].binding=0;
  shadow_bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  shadow_bindings[0].descriptorCount=1;
  shadow_bindings[0].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
  shadow_bindings[1].binding=1;
  shadow_bindings[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  shadow_bindings[1].descriptorCount=1;
  shadow_bindings[1].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
  for(std::uint32_t binding=2;binding<4U;++binding){
    shadow_bindings[binding].binding=binding;
    shadow_bindings[binding].descriptorType=
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadow_bindings[binding].descriptorCount=1;
    shadow_bindings[binding].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  shadow_bindings[4].binding=4;
  shadow_bindings[4].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  shadow_bindings[4].descriptorCount=1;
  shadow_bindings[4].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
  shadow_bindings[5].binding=5;
  shadow_bindings[5].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  shadow_bindings[5].descriptorCount=1;
  shadow_bindings[5].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo descriptor_layout{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptor_layout.bindingCount=static_cast<std::uint32_t>(
      shadow_bindings.size());
  descriptor_layout.pBindings=shadow_bindings.data();
  if(vkCreateDescriptorSetLayout(device_,&descriptor_layout,nullptr,
                                 &descriptor_set_layout_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create shadow descriptor layout");

  std::array<VkDescriptorSetLayoutBinding,17> composite_bindings{};
  for(std::uint32_t binding=0;binding<composite_bindings.size();++binding){
    composite_bindings[binding].binding=binding;
    composite_bindings[binding].descriptorType=(binding==7U||binding==10U)?
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    composite_bindings[binding].descriptorCount=1;
    composite_bindings[binding].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo composite_descriptor_layout{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  composite_descriptor_layout.bindingCount=
      static_cast<std::uint32_t>(composite_bindings.size());
  composite_descriptor_layout.pBindings=composite_bindings.data();
  if(vkCreateDescriptorSetLayout(device_,&composite_descriptor_layout,nullptr,
                                 &composite_descriptor_set_layout_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene composite descriptor layout");

  std::array<VkDescriptorSetLayoutBinding,26> atmosphere_bindings{};
  for(std::uint32_t binding=0;binding<atmosphere_bindings.size();++binding){
    atmosphere_bindings[binding].binding=binding;
    atmosphere_bindings[binding].descriptorType=(binding==9U||binding==11U)?
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        (binding<5U||binding==8U||binding==10U||binding>=13U)?
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        (binding==6U||binding==12U?
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    atmosphere_bindings[binding].descriptorCount=1;
    atmosphere_bindings[binding].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo atmosphere_descriptor_layout{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  atmosphere_descriptor_layout.bindingCount=
      static_cast<std::uint32_t>(atmosphere_bindings.size());
  atmosphere_descriptor_layout.pBindings=atmosphere_bindings.data();
  if(vkCreateDescriptorSetLayout(device_,&atmosphere_descriptor_layout,nullptr,
                                 &atmosphere_descriptor_set_layout_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create atmosphere descriptor layout");

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter=VK_FILTER_LINEAR;
  sampler.minFilter=VK_FILTER_LINEAR;
  sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  sampler.maxLod=1.0F;
  if(vkCreateSampler(device_,&sampler,nullptr,&shadow_sampler_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create shadow sampler");
  sampler.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if(vkCreateSampler(device_,&sampler,nullptr,&scene_sampler_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create HDR scene sampler");
  sampler.magFilter=VK_FILTER_NEAREST;
  sampler.minFilter=VK_FILTER_NEAREST;
  if(vkCreateSampler(device_,&sampler,nullptr,&depth_sampler_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene depth sampler");

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 28};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS) throw std::runtime_error("unable to create scene pipeline layout");
  layout.setLayoutCount=1;
  layout.pSetLayouts=&descriptor_set_layout_;
  if(vkCreatePipelineLayout(device_,&layout,nullptr,&shaded_pipeline_layout_)!=
     VK_SUCCESS)
    throw std::runtime_error("unable to create shaded scene pipeline layout");
  VkPipelineLayoutCreateInfo composite_layout{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  composite_layout.setLayoutCount=1;
  composite_layout.pSetLayouts=&composite_descriptor_set_layout_;
  if(vkCreatePipelineLayout(device_,&composite_layout,nullptr,
                            &composite_pipeline_layout_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene composite pipeline layout");
  VkPushConstantRange atmosphere_push{VK_SHADER_STAGE_COMPUTE_BIT,0,
                                      sizeof(std::uint32_t)*4U};
  VkPipelineLayoutCreateInfo atmosphere_layout{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  atmosphere_layout.setLayoutCount=1;
  atmosphere_layout.pSetLayouts=&atmosphere_descriptor_set_layout_;
  atmosphere_layout.pushConstantRangeCount=1;
  atmosphere_layout.pPushConstantRanges=&atmosphere_push;
  if(vkCreatePipelineLayout(device_,&atmosphere_layout,nullptr,
                            &atmosphere_pipeline_layout_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create atmosphere pipeline layout");

  const auto vertex_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/scene.vert.spv");
  const auto fragment_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/scene.frag.spv");
  const auto wire_fragment_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/wire.frag.spv");
  const auto edge_vertex_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/edge.vert.spv");
  const auto edge_fragment_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/edge.frag.spv");
  const auto shadow_vertex_shader = shader_module(device_, TETRA_VIEWER_SHADER_DIR "/shadow.vert.spv");
  const auto sky_vertex_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/sky.vert.spv");
  const auto sky_fragment_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/sky.frag.spv");
  const auto fullscreen_vertex_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/fullscreen.vert.spv");
  const auto tone_map_fragment_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/tone_map.frag.spv");
  const auto faithful_tone_map_fragment_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/tone_map_faithful.frag.spv");
  const auto atmosphere_compute_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/atmosphere.comp.spv");
  const auto faithful_atmosphere_compute_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/atmosphere_faithful.comp.spv");
  const auto reference_hillaire_atmosphere_compute_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR
          "/atmosphere_reference_hillaire.comp.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main"};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, "main"};
  VkPipelineShaderStageCreateInfo wire_stages[2]{stages[0], stages[1]};
  wire_stages[1].module = wire_fragment_shader;
  VkPipelineShaderStageCreateInfo edge_stages[2]{};
  edge_stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, edge_vertex_shader, "main"};
  edge_stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, edge_fragment_shader, "main"};
  VkVertexInputBindingDescription binding{0, sizeof(SceneVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[7]{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24}, {3, 0, VK_FORMAT_R32G32_SFLOAT, 36}, {4, 0, VK_FORMAT_R32G32B32_SFLOAT, 48}, {5, 0, VK_FORMAT_R32_SFLOAT, 44}, {6, 0, VK_FORMAT_R32G32B32_SFLOAT, 60}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1; vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 7; vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = 1; viewport.scissorCount = 1;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend_attachment{}; blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT; blend_attachment.blendEnable = VK_FALSE;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &scene_colour_format_; rendering.depthAttachmentFormat = depth_format_;
  const auto create_pipeline = [&](const VkPipelineShaderStageCreateInfo* pipeline_stages,
                                   VkPolygonMode polygon_mode, bool depth_test,
                                   bool depth_overlay,
                                   bool alpha_blend, bool offset_fill,
                                   VkPipelineLayout pipeline_layout,
                                   VkSampleCountFlagBits sample_count,
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
    raster.depthBiasConstantFactor = offset_fill ? -1.0F : 0.0F;
    raster.depthBiasSlopeFactor = offset_fill ? -1.0F : 0.0F;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = depth_overlay ? VK_FALSE : VK_TRUE;
    depth.depthCompareOp=depth_compare(main_scene_depth_convention,depth_overlay);
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
    VkPipelineMultisampleStateCreateInfo pipeline_multisample=multisample;
    pipeline_multisample.rasterizationSamples=sample_count;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering; pipeline.stageCount = 2; pipeline.pStages = pipeline_stages; pipeline.pVertexInputState = &vertex_input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &pipeline_multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &pipeline_blend; pipeline.pDynamicState = &dynamic; pipeline.layout = pipeline_layout;
    const auto result=vkCreateGraphicsPipelines(
        device_,VK_NULL_HANDLE,1,&pipeline,nullptr,target);
    if(result!=VK_SUCCESS){
      const char* name=target==&triangle_pipeline_?"triangles":
          (target==&triangle_wire_pipeline_?"triangle wire":
           (target==&line_pipeline_?"hierarchy lines":
            (target==&editor_line_pipeline_?"editor lines":"sky")));
      throw std::runtime_error(std::string{"unable to create scene pipeline: "}+
                               name+" (VkResult "+
                               std::to_string(static_cast<int>(result))+')');
    }
  };
  create_pipeline(stages, VK_POLYGON_MODE_FILL, true, false, false, true,
                  shaded_pipeline_layout_,VK_SAMPLE_COUNT_1_BIT,
                  &triangle_pipeline_);
  // The surface wire pass rasterizes the exact same triangle vertices as the
  // opaque pass. Each edge is therefore a native one-pixel line between its
  // two endpoints, with identical depth interpolation and no depth pull.
  create_pipeline(wire_stages,VK_POLYGON_MODE_LINE,true,true,false,false,
                  pipeline_layout_,VK_SAMPLE_COUNT_1_BIT,
                  &triangle_wire_pipeline_);
  // Hierarchy edges are independent segments, expanded into screen-space
  // ribbons by edge.vert. They retain alpha coverage for antialiasing.
  create_pipeline(edge_stages,VK_POLYGON_MODE_FILL,true,true,true,false,
                  pipeline_layout_,VK_SAMPLE_COUNT_1_BIT,&line_pipeline_);
  // Editor objects and manipulation handles remain visible when they overlap
  // the inspected mesh, as in conventional DCC applications. They use their
  // own buffer and pipeline so mesh-edge visibility remains depth correct.
  create_pipeline(edge_stages,VK_POLYGON_MODE_FILL,false,true,true,false,
                  pipeline_layout_,VK_SAMPLE_COUNT_1_BIT,
                  &editor_line_pipeline_);
  VkPipelineShaderStageCreateInfo sky_stages[2]{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
       VK_SHADER_STAGE_VERTEX_BIT,sky_vertex_shader,"main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
       VK_SHADER_STAGE_FRAGMENT_BIT,sky_fragment_shader,"main"}};
  create_pipeline(sky_stages,VK_POLYGON_MODE_FILL,false,false,false,false,
                  pipeline_layout_,VK_SAMPLE_COUNT_1_BIT,&sky_pipeline_);
  if(terrain_msaa_supported_){
    create_pipeline(stages,VK_POLYGON_MODE_FILL,true,false,false,true,
                    shaded_pipeline_layout_,VK_SAMPLE_COUNT_4_BIT,
                    &msaa_triangle_pipeline_);
    create_pipeline(wire_stages,VK_POLYGON_MODE_LINE,true,true,false,false,
                    pipeline_layout_,VK_SAMPLE_COUNT_4_BIT,
                    &msaa_triangle_wire_pipeline_);
    create_pipeline(edge_stages,VK_POLYGON_MODE_FILL,true,true,true,false,
                    pipeline_layout_,VK_SAMPLE_COUNT_4_BIT,
                    &msaa_line_pipeline_);
    create_pipeline(edge_stages,VK_POLYGON_MODE_FILL,false,true,true,false,
                    pipeline_layout_,VK_SAMPLE_COUNT_4_BIT,
                    &msaa_editor_line_pipeline_);
    create_pipeline(sky_stages,VK_POLYGON_MODE_FILL,false,false,false,false,
                    pipeline_layout_,VK_SAMPLE_COUNT_4_BIT,
                    &msaa_sky_pipeline_);
  }

  VkPipelineShaderStageCreateInfo composite_stages[2]{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
       VK_SHADER_STAGE_VERTEX_BIT,fullscreen_vertex_shader,"main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
       VK_SHADER_STAGE_FRAGMENT_BIT,tone_map_fragment_shader,"main"}};
  VkPipelineVertexInputStateCreateInfo empty_vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo composite_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  composite_assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineRasterizationStateCreateInfo composite_raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  composite_raster.polygonMode=VK_POLYGON_MODE_FILL;
  composite_raster.cullMode=VK_CULL_MODE_NONE;
  composite_raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
  composite_raster.lineWidth=1.0F;
  VkPipelineRenderingCreateInfo composite_rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  composite_rendering.colorAttachmentCount=1;
  composite_rendering.pColorAttachmentFormats=&colour_format_;
  VkGraphicsPipelineCreateInfo composite_pipeline{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  composite_pipeline.pNext=&composite_rendering;
  composite_pipeline.stageCount=2;
  composite_pipeline.pStages=composite_stages;
  composite_pipeline.pVertexInputState=&empty_vertex_input;
  composite_pipeline.pInputAssemblyState=&composite_assembly;
  composite_pipeline.pViewportState=&viewport;
  composite_pipeline.pRasterizationState=&composite_raster;
  composite_pipeline.pMultisampleState=&multisample;
  composite_pipeline.pColorBlendState=&blend;
  composite_pipeline.pDynamicState=&dynamic;
  composite_pipeline.layout=composite_pipeline_layout_;
  if(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&composite_pipeline,
                               nullptr,&composite_pipeline_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create HDR scene composite pipeline");
  composite_stages[1].module=faithful_tone_map_fragment_shader;
  if(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&composite_pipeline,
                               nullptr,&faithful_composite_pipeline_)!=VK_SUCCESS)
    throw std::runtime_error(
        "unable to create faithful HDR scene composite pipeline");

  VkPipelineShaderStageCreateInfo atmosphere_stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  atmosphere_stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
  atmosphere_stage.module=atmosphere_compute_shader;
  atmosphere_stage.pName="main";
  VkComputePipelineCreateInfo atmosphere_pipeline{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  atmosphere_pipeline.stage=atmosphere_stage;
  atmosphere_pipeline.layout=atmosphere_pipeline_layout_;
  if(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&atmosphere_pipeline,
                              nullptr,&atmosphere_pipeline_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create atmosphere compute pipeline");
  atmosphere_pipeline.stage.module=faithful_atmosphere_compute_shader;
  if(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&atmosphere_pipeline,
                              nullptr,&faithful_atmosphere_pipeline_)!=VK_SUCCESS)
    throw std::runtime_error(
        "unable to create faithful atmosphere compute pipeline");
  atmosphere_pipeline.stage.module=reference_hillaire_atmosphere_compute_shader;
  if(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&atmosphere_pipeline,
                              nullptr,
                              &reference_hillaire_atmosphere_pipeline_)!=
     VK_SUCCESS)
    throw std::runtime_error(
        "unable to create reference Hillaire atmosphere compute pipeline");

  VkPipelineShaderStageCreateInfo shadow_stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
      VK_SHADER_STAGE_VERTEX_BIT,shadow_vertex_shader,"main"};
  VkPipelineInputAssemblyStateCreateInfo shadow_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  shadow_assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineRasterizationStateCreateInfo shadow_raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  shadow_raster.polygonMode=VK_POLYGON_MODE_FILL;
  shadow_raster.cullMode=VK_CULL_MODE_NONE;
  shadow_raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
  shadow_raster.lineWidth=1.0F;
  shadow_raster.depthBiasEnable=VK_TRUE;
  shadow_raster.depthBiasConstantFactor=0.8125F;
  shadow_raster.depthBiasSlopeFactor=1.21875F;
  const VkDynamicState shadow_dynamic_states[]{
      VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo shadow_dynamic=dynamic;
  shadow_dynamic.dynamicStateCount=3U;
  shadow_dynamic.pDynamicStates=shadow_dynamic_states;
  VkPipelineDepthStencilStateCreateInfo shadow_depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  shadow_depth.depthTestEnable=VK_TRUE;
  shadow_depth.depthWriteEnable=VK_TRUE;
  shadow_depth.depthCompareOp=depth_compare(shadow_map_depth_convention);
  VkPipelineColorBlendStateCreateInfo shadow_blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  VkPipelineRenderingCreateInfo shadow_rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  shadow_rendering.depthAttachmentFormat=depth_format_;
  VkGraphicsPipelineCreateInfo shadow_pipeline{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  shadow_pipeline.pNext=&shadow_rendering;
  shadow_pipeline.stageCount=1;
  shadow_pipeline.pStages=&shadow_stage;
  shadow_pipeline.pVertexInputState=&vertex_input;
  shadow_pipeline.pInputAssemblyState=&shadow_assembly;
  shadow_pipeline.pViewportState=&viewport;
  shadow_pipeline.pRasterizationState=&shadow_raster;
  shadow_pipeline.pMultisampleState=&multisample;
  shadow_pipeline.pDepthStencilState=&shadow_depth;
  shadow_pipeline.pColorBlendState=&shadow_blend;
  shadow_pipeline.pDynamicState=&shadow_dynamic;
  shadow_pipeline.layout=pipeline_layout_;
  if(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&shadow_pipeline,nullptr,
                               &shadow_pipeline_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create shadow pipeline");
  vkDestroyShaderModule(device_, vertex_shader, nullptr); vkDestroyShaderModule(device_, fragment_shader, nullptr);
  vkDestroyShaderModule(device_, wire_fragment_shader, nullptr);
  vkDestroyShaderModule(device_, edge_vertex_shader, nullptr); vkDestroyShaderModule(device_, edge_fragment_shader, nullptr);
  vkDestroyShaderModule(device_,shadow_vertex_shader,nullptr);
  vkDestroyShaderModule(device_,sky_vertex_shader,nullptr);
  vkDestroyShaderModule(device_,sky_fragment_shader,nullptr);
  vkDestroyShaderModule(device_,fullscreen_vertex_shader,nullptr);
  vkDestroyShaderModule(device_,tone_map_fragment_shader,nullptr);
  vkDestroyShaderModule(device_,faithful_tone_map_fragment_shader,nullptr);
  vkDestroyShaderModule(device_,atmosphere_compute_shader,nullptr);
  vkDestroyShaderModule(device_,faithful_atmosphere_compute_shader,nullptr);
  vkDestroyShaderModule(
      device_,reference_hillaire_atmosphere_compute_shader,nullptr);
}

void SceneRenderer::recreate(VkExtent2D extent, std::uint32_t image_count,
                             AtmosphereQuality quality,
                             std::uint32_t screen_resolution_divisor,
                             bool terrain_msaa) {
  quality_settings_=atmosphere_quality_settings(quality);
  screen_resolution_divisor_=std::clamp(screen_resolution_divisor,2U,4U);
  terrain_msaa_enabled_=terrain_msaa&&terrain_msaa_supported_;
  atmosphere_dispatch_counts_={};
  dynamic_sun_shadow_phase_=0U;
  dynamic_sun_was_active_=false;
  latest_atmosphere_probe_={};
  latest_capture_={};
  if(timing_query_pool_!=VK_NULL_HANDLE){
    vkDestroyQueryPool(device_,timing_query_pool_,nullptr);
    timing_query_pool_=VK_NULL_HANDLE;
  }
  for (auto& depth : depth_images_) { vkDestroyImageView(device_, depth.view, nullptr); vkDestroyImage(device_, depth.image, nullptr); vkFreeMemory(device_, depth.memory, nullptr); }
  for(auto& colour:scene_colour_images_){
    vkDestroyImageView(device_,colour.view,nullptr);
    vkDestroyImage(device_,colour.image,nullptr);
    vkFreeMemory(device_,colour.memory,nullptr);
  }
  vkDestroyImageView(device_,msaa_depth_image_.view,nullptr);
  vkDestroyImage(device_,msaa_depth_image_.image,nullptr);
  vkFreeMemory(device_,msaa_depth_image_.memory,nullptr);
  msaa_depth_image_={};
  vkDestroyImageView(device_,msaa_colour_image_.view,nullptr);
  vkDestroyImage(device_,msaa_colour_image_.image,nullptr);
  vkFreeMemory(device_,msaa_colour_image_.memory,nullptr);
  msaa_colour_image_={};
  for(auto& shadow:shadow_images_){
    for(auto view:shadow.layer_views)vkDestroyImageView(device_,view,nullptr);
    vkDestroyImageView(device_,shadow.view,nullptr);
    vkDestroyImage(device_,shadow.image,nullptr);
    vkFreeMemory(device_,shadow.memory,nullptr);
    vkDestroyBuffer(device_,shadow.uniform_buffer,nullptr);
    vkFreeMemory(device_,shadow.uniform_memory,nullptr);
    vkDestroyBuffer(device_,shadow.minmax_buffer,nullptr);
    vkFreeMemory(device_,shadow.minmax_memory,nullptr);
    vkDestroyBuffer(device_,shadow.epipolar_diagnostic_buffer,nullptr);
    vkFreeMemory(device_,shadow.epipolar_diagnostic_memory,nullptr);
  }
  for(auto* image:{&atmosphere_lookups_.transmittance,
                   &atmosphere_lookups_.multiple_scattering,
                   &atmosphere_lookups_.sky_view,
                   &atmosphere_lookups_.sky_irradiance,
                   &atmosphere_lookups_.aerial_scattering,
                   &atmosphere_lookups_.aerial_transmittance,
                   &atmosphere_lookups_.long_shadow}){
      vkDestroyImageView(device_,image->view,nullptr);
      vkDestroyImage(device_,image->image,nullptr);
      vkFreeMemory(device_,image->memory,nullptr);
  }
  atmosphere_lookups_={};
  for(auto& frame:atmosphere_frames_){
    vkDestroyBuffer(device_,frame.uniform_buffer,nullptr);
    vkFreeMemory(device_,frame.uniform_memory,nullptr);
    vkDestroyBuffer(device_,frame.probe_buffer,nullptr);
    vkFreeMemory(device_,frame.probe_memory,nullptr);
    for(auto* image:{&frame.screen_scattering,&frame.screen_transmittance,
                     &frame.screen_endpoint,&frame.froxel_scattering,
                     &frame.froxel_transmittance}){
      vkDestroyImageView(device_,image->view,nullptr);
      vkDestroyImage(device_,image->image,nullptr);
      vkFreeMemory(device_,image->memory,nullptr);
    }
    for(std::size_t history=0;history<2U;++history)
      for(auto* image:{&frame.history_scattering[history],
                       &frame.history_transmittance[history],
                       &frame.history_endpoint[history],
                       &frame.history_visibility[history]}){
        vkDestroyImageView(device_,image->view,nullptr);
        vkDestroyImage(device_,image->image,nullptr);
        vkFreeMemory(device_,image->memory,nullptr);
      }
  }
  for(auto& capture:capture_frames_){
    vkDestroyImageView(device_,capture.view,nullptr);
    vkDestroyImage(device_,capture.image,nullptr);
    vkFreeMemory(device_,capture.image_memory,nullptr);
    vkDestroyBuffer(device_,capture.buffer,nullptr);
    vkFreeMemory(device_,capture.buffer_memory,nullptr);
    vkDestroyBuffer(device_,capture.depth_buffer,nullptr);
    vkFreeMemory(device_,capture.depth_buffer_memory,nullptr);
  }
  atmosphere_frames_.clear();
  capture_frames_.clear();
  shadow_images_.clear();
  descriptor_sets_.clear();
  composite_descriptor_sets_.clear();
  if(descriptor_pool_!=VK_NULL_HANDLE){
    vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
    descriptor_pool_=VK_NULL_HANDLE;
  }
  VkQueryPoolCreateInfo timing_pool{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  timing_pool.queryType=VK_QUERY_TYPE_TIMESTAMP;
  timing_pool.queryCount=image_count*8U;
  if(vkCreateQueryPool(device_,&timing_pool,nullptr,&timing_query_pool_)!=
     VK_SUCCESS)
    throw std::runtime_error("unable to create scene timing query pool");
  timing_queries_written_.assign(image_count,false);
  gpu_timings_={};
  atmosphere_shadow_map_status_={};
  const auto frames=static_cast<std::size_t>(image_count);
  scene_target_allocation_bytes_=frames*
      static_cast<std::size_t>(extent.width)*extent.height*(8U+4U);
  if(terrain_msaa_enabled_)
    scene_target_allocation_bytes_+=
        static_cast<std::size_t>(extent.width)*extent.height*4U*(8U+4U);
  const std::size_t lookup_bytes=
      quality_settings_.transmittance_width*
          quality_settings_.transmittance_height*8U+
      quality_settings_.multiple_scattering_size*
          quality_settings_.multiple_scattering_size*8U+
      quality_settings_.sky_width*quality_settings_.sky_height*8U+
      quality_settings_.irradiance_width*
          quality_settings_.irradiance_height*8U+
      quality_settings_.long_shadow_width*
          quality_settings_.long_shadow_height*8U+
      2U*quality_settings_.aerial_width*quality_settings_.aerial_height*
          quality_settings_.aerial_depth*8U;
  const std::size_t shadow_bytes=shadow_map_layer_count*
      quality_settings_.shadow_resolution*
      quality_settings_.shadow_resolution*4U;
  const std::size_t minmax_bytes=shadow_minmax_element_count(
      quality_settings_.atmosphere_shadow_resolution)*sizeof(float)*2U;
  constexpr std::size_t epipolar_diagnostic_bytes=sizeof(std::uint32_t)*4U;
  const std::size_t reconstructed_screen_bytes=
      static_cast<std::size_t>((extent.width+screen_resolution_divisor_-1U)/
                               screen_resolution_divisor_)*
      ((extent.height+screen_resolution_divisor_-1U)/
       screen_resolution_divisor_)*((8U+8U+16U)*3U+8U*2U);
  atmosphere_allocation_bytes_=lookup_bytes+frames*(
      shadow_bytes+minmax_bytes+epipolar_diagnostic_bytes+
      reconstructed_screen_bytes+32U*32U*32U*16U);
  depth_images_.assign(image_count, {});
  for (auto& depth : depth_images_) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = depth_format_; image.extent = {extent.width, extent.height, 1}; image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if(vkCreateImage(device_, &image, nullptr, &depth.image)!=VK_SUCCESS)
      throw std::runtime_error("unable to create sampleable scene depth image");
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device_, depth.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memory_type(physical_device_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_, &allocation, nullptr, &depth.memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate scene depth image");
    if(vkBindImageMemory(device_, depth.image, depth.memory, 0)!=VK_SUCCESS)
      throw std::runtime_error("unable to bind scene depth image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = depth.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = depth_format_; view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; view.subresourceRange.levelCount = 1; view.subresourceRange.layerCount = 1;
    if(vkCreateImageView(device_, &view, nullptr, &depth.view)!=VK_SUCCESS)
      throw std::runtime_error("unable to create scene depth view");
  }

  scene_colour_images_.assign(image_count,{});
  for(auto& colour:scene_colour_images_){
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D;
    image.format=scene_colour_format_;
    image.extent={extent.width,extent.height,1U};
    image.mipLevels=1;
    image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(vkCreateImage(device_,&image,nullptr,&colour.image)!=VK_SUCCESS)
      throw std::runtime_error("unable to create HDR scene colour image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_,colour.image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&colour.memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate HDR scene colour image");
    if(vkBindImageMemory(device_,colour.image,colour.memory,0)!=VK_SUCCESS)
      throw std::runtime_error("unable to bind HDR scene colour image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=colour.image;
    // tone_map.frag binds this image as sampler2D. A 2D-array view is not
    // compatible and silently sampled as the clear colour on MoltenVK.
    view.viewType=scene_colour_sample_dimension==
        SceneSampledImageDimension::two_d?VK_IMAGE_VIEW_TYPE_2D:
                                         VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format=scene_colour_format_;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount=1;
    view.subresourceRange.layerCount=1;
    if(vkCreateImageView(device_,&view,nullptr,&colour.view)!=VK_SUCCESS)
      throw std::runtime_error("unable to create HDR scene colour view");
  }

  if(terrain_msaa_enabled_){
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D;
    image.extent={extent.width,extent.height,1U};
    image.mipLevels=1U;
    image.arrayLayers=1U;
    image.samples=VK_SAMPLE_COUNT_4_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;

    image.format=scene_colour_format_;
    image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if(vkCreateImage(device_,&image,nullptr,&msaa_colour_image_.image)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to create multisampled scene colour");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_,msaa_colour_image_.image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,
                        &msaa_colour_image_.memory)!=VK_SUCCESS||
       vkBindImageMemory(device_,msaa_colour_image_.image,
                         msaa_colour_image_.memory,0)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate multisampled scene colour");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=msaa_colour_image_.image;
    view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    view.format=scene_colour_format_;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount=1U;
    view.subresourceRange.layerCount=1U;
    if(vkCreateImageView(device_,&view,nullptr,&msaa_colour_image_.view)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to create multisampled scene colour view");

    image.format=depth_format_;
    image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if(vkCreateImage(device_,&image,nullptr,&msaa_depth_image_.image)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to create multisampled scene depth");
    vkGetImageMemoryRequirements(device_,msaa_depth_image_.image,&requirements);
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,
                        &msaa_depth_image_.memory)!=VK_SUCCESS||
       vkBindImageMemory(device_,msaa_depth_image_.image,
                         msaa_depth_image_.memory,0)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate multisampled scene depth");
    view.image=msaa_depth_image_.image;
    view.format=depth_format_;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    if(vkCreateImageView(device_,&view,nullptr,&msaa_depth_image_.view)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to create multisampled scene depth view");
  }

  capture_frames_.resize(image_count);

  const auto make_atmosphere_image=[this](AtmosphereImage& destination,
                                           VkImageType image_type,
                                           VkImageViewType view_type,
                                           VkExtent3D image_extent,
                                           VkFormat format=
                                               VK_FORMAT_R16G16B16A16_SFLOAT){
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=image_type;
    image.format=format;
    image.extent=image_extent;
    image.mipLevels=1;
    image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(vkCreateImage(device_,&image,nullptr,&destination.image)!=VK_SUCCESS)
      throw std::runtime_error("unable to create atmosphere lookup image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_,destination.image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&destination.memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate atmosphere lookup image");
    if(vkBindImageMemory(device_,destination.image,destination.memory,0)!=VK_SUCCESS)
      throw std::runtime_error("unable to bind atmosphere lookup image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=destination.image;
    view.viewType=view_type;
    view.format=format;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount=1;
    view.subresourceRange.layerCount=1;
    if(vkCreateImageView(device_,&view,nullptr,&destination.view)!=VK_SUCCESS)
      throw std::runtime_error("unable to create atmosphere lookup view");
  };
  atmosphere_frames_.resize(image_count);
  make_atmosphere_image(atmosphere_lookups_.transmittance,VK_IMAGE_TYPE_2D,
                        VK_IMAGE_VIEW_TYPE_2D,
                        {quality_settings_.transmittance_width,
                         quality_settings_.transmittance_height,1U});
  make_atmosphere_image(atmosphere_lookups_.multiple_scattering,
                        VK_IMAGE_TYPE_2D,VK_IMAGE_VIEW_TYPE_2D,
                        {quality_settings_.multiple_scattering_size,
                         quality_settings_.multiple_scattering_size,1U});
  make_atmosphere_image(atmosphere_lookups_.sky_view,VK_IMAGE_TYPE_2D,
                        VK_IMAGE_VIEW_TYPE_2D,{quality_settings_.sky_width,
                                              quality_settings_.sky_height,1U});
  make_atmosphere_image(atmosphere_lookups_.sky_irradiance,VK_IMAGE_TYPE_2D,
                        VK_IMAGE_VIEW_TYPE_2D,
                        {quality_settings_.irradiance_width,
                         quality_settings_.irradiance_height,1U});
  make_atmosphere_image(atmosphere_lookups_.aerial_scattering,
                        VK_IMAGE_TYPE_3D,VK_IMAGE_VIEW_TYPE_3D,
                        {quality_settings_.aerial_width,
                         quality_settings_.aerial_height,
                         quality_settings_.aerial_depth});
  make_atmosphere_image(atmosphere_lookups_.aerial_transmittance,
                        VK_IMAGE_TYPE_3D,VK_IMAGE_VIEW_TYPE_3D,
                        {quality_settings_.aerial_width,
                         quality_settings_.aerial_height,
                         quality_settings_.aerial_depth});
  make_atmosphere_image(atmosphere_lookups_.long_shadow,VK_IMAGE_TYPE_2D,
                        VK_IMAGE_VIEW_TYPE_2D,
                        {quality_settings_.long_shadow_width,
                         quality_settings_.long_shadow_height,1U});
  for(auto& frame:atmosphere_frames_){
    const VkExtent3D screen_extent{
        (extent.width+screen_resolution_divisor_-1U)/
            screen_resolution_divisor_,
        (extent.height+screen_resolution_divisor_-1U)/
            screen_resolution_divisor_,1U};
    make_atmosphere_image(frame.screen_scattering,VK_IMAGE_TYPE_2D,
                          VK_IMAGE_VIEW_TYPE_2D,screen_extent);
    make_atmosphere_image(frame.screen_transmittance,VK_IMAGE_TYPE_2D,
                          VK_IMAGE_VIEW_TYPE_2D,screen_extent);
    make_atmosphere_image(frame.screen_endpoint,VK_IMAGE_TYPE_2D,
                          VK_IMAGE_VIEW_TYPE_2D,screen_extent,
                          VK_FORMAT_R32G32B32A32_SFLOAT);
    make_atmosphere_image(frame.froxel_scattering,VK_IMAGE_TYPE_3D,
                          VK_IMAGE_VIEW_TYPE_3D,{32U,32U,32U});
    make_atmosphere_image(frame.froxel_transmittance,VK_IMAGE_TYPE_3D,
                          VK_IMAGE_VIEW_TYPE_3D,{32U,32U,32U});
    for(std::size_t history=0;history<2U;++history){
      make_atmosphere_image(frame.history_scattering[history],
                            VK_IMAGE_TYPE_2D,VK_IMAGE_VIEW_TYPE_2D,
                            screen_extent);
      make_atmosphere_image(frame.history_transmittance[history],
                            VK_IMAGE_TYPE_2D,VK_IMAGE_VIEW_TYPE_2D,
                            screen_extent);
      make_atmosphere_image(frame.history_endpoint[history],VK_IMAGE_TYPE_2D,
                            VK_IMAGE_VIEW_TYPE_2D,screen_extent,
                            VK_FORMAT_R32G32B32A32_SFLOAT);
      make_atmosphere_image(frame.history_visibility[history],
                            VK_IMAGE_TYPE_2D,VK_IMAGE_VIEW_TYPE_2D,
                            screen_extent,VK_FORMAT_R32G32_UINT);
    }
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=sizeof(float)*96U;
    buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if(vkCreateBuffer(device_,&buffer,nullptr,&frame.uniform_buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create atmosphere uniform buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_,frame.uniform_buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&frame.uniform_memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate atmosphere uniform buffer");
    if(vkBindBufferMemory(device_,frame.uniform_buffer,frame.uniform_memory,0)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to bind atmosphere uniform buffer");
    VkBufferCreateInfo probe{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    probe.size=sizeof(float)*4U*atmosphere_numeric_probe_value_count;
    probe.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if(vkCreateBuffer(device_,&probe,nullptr,&frame.probe_buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create atmosphere probe buffer");
    vkGetBufferMemoryRequirements(device_,frame.probe_buffer,&requirements);
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&frame.probe_memory)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to allocate atmosphere probe buffer");
    if(vkBindBufferMemory(device_,frame.probe_buffer,frame.probe_memory,0)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to bind atmosphere probe buffer");
  }

  const std::uint32_t shadow_extent=quality_settings_.shadow_resolution;
  shadow_images_.resize(image_count);
  for(auto& shadow:shadow_images_){
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D;
    image.format=depth_format_;
    image.extent={shadow_extent,shadow_extent,1U};
    image.mipLevels=1;
    image.arrayLayers=static_cast<std::uint32_t>(shadow_map_layer_count);
    image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_SAMPLED_BIT;
    if(vkCreateImage(device_,&image,nullptr,&shadow.image)!=VK_SUCCESS)
      throw std::runtime_error("unable to create sun shadow image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_,shadow.image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&shadow.memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate sun shadow image");
    if(vkBindImageMemory(device_,shadow.image,shadow.memory,0)!=VK_SUCCESS)
      throw std::runtime_error("unable to bind sun shadow image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=shadow.image;
    view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format=depth_format_;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount=1;
    view.subresourceRange.layerCount=static_cast<std::uint32_t>(
        shadow_map_layer_count);
    if(vkCreateImageView(device_,&view,nullptr,&shadow.view)!=VK_SUCCESS)
      throw std::runtime_error("unable to create sun shadow view");
    for(std::uint32_t layer=0;layer<shadow_map_layer_count;++layer){
      view.viewType=VK_IMAGE_VIEW_TYPE_2D;
      view.subresourceRange.baseArrayLayer=layer;
      view.subresourceRange.layerCount=1;
      if(vkCreateImageView(device_,&view,nullptr,
                           &shadow.layer_views[layer])!=VK_SUCCESS)
        throw std::runtime_error("unable to create sun shadow cascade view");
    }
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=sizeof(float)*(16U*shadow_map_layer_count+12U);
    buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if(vkCreateBuffer(device_,&buffer,nullptr,&shadow.uniform_buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create shadow cascade uniform buffer");
    vkGetBufferMemoryRequirements(device_,shadow.uniform_buffer,&requirements);
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&shadow.uniform_memory)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to allocate shadow cascade uniforms");
    if(vkBindBufferMemory(device_,shadow.uniform_buffer,shadow.uniform_memory,0)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to bind shadow cascade uniforms");
    buffer={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=shadow_minmax_element_count(
        quality_settings_.atmosphere_shadow_resolution)*sizeof(float)*2U;
    buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if(vkCreateBuffer(device_,&buffer,nullptr,&shadow.minmax_buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create atmosphere shadow min-max buffer");
    vkGetBufferMemoryRequirements(device_,shadow.minmax_buffer,&requirements);
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(physical_device_,
        requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,&shadow.minmax_memory)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to allocate atmosphere shadow min-max buffer");
    if(vkBindBufferMemory(device_,shadow.minmax_buffer,shadow.minmax_memory,0)!=
       VK_SUCCESS)
      throw std::runtime_error("unable to bind atmosphere shadow min-max buffer");
    buffer={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=sizeof(std::uint32_t)*4U;
    buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(vkCreateBuffer(device_,&buffer,nullptr,
                      &shadow.epipolar_diagnostic_buffer)!=VK_SUCCESS)
      throw std::runtime_error("unable to create epipolar diagnostic buffer");
    vkGetBufferMemoryRequirements(
        device_,shadow.epipolar_diagnostic_buffer,&requirements);
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(
        physical_device_,requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(device_,&allocation,nullptr,
                        &shadow.epipolar_diagnostic_memory)!=VK_SUCCESS)
      throw std::runtime_error("unable to allocate epipolar diagnostics");
    if(vkBindBufferMemory(device_,shadow.epipolar_diagnostic_buffer,
                          shadow.epipolar_diagnostic_memory,0)!=VK_SUCCESS)
      throw std::runtime_error("unable to bind epipolar diagnostics");
  }

  const std::array<VkDescriptorPoolSize,4> pool_sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           image_count*21U},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,image_count*20U},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,image_count*6U},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,image_count*2U}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets=image_count*3U;
  pool.poolSizeCount=static_cast<std::uint32_t>(pool_sizes.size());
  pool.pPoolSizes=pool_sizes.data();
  if(vkCreateDescriptorPool(device_,&pool,nullptr,&descriptor_pool_)!=VK_SUCCESS)
    throw std::runtime_error("unable to create sun shadow descriptor pool");
  descriptor_sets_.resize(image_count);
  std::vector<VkDescriptorSetLayout> layouts(image_count,descriptor_set_layout_);
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool=descriptor_pool_;
  allocate.descriptorSetCount=image_count;
  allocate.pSetLayouts=layouts.data();
  if(vkAllocateDescriptorSets(device_,&allocate,descriptor_sets_.data())!=VK_SUCCESS)
    throw std::runtime_error("unable to allocate sun shadow descriptors");
  composite_descriptor_sets_.resize(image_count);
  layouts.assign(image_count,composite_descriptor_set_layout_);
  allocate.pSetLayouts=layouts.data();
  if(vkAllocateDescriptorSets(device_,&allocate,
                              composite_descriptor_sets_.data())!=VK_SUCCESS)
    throw std::runtime_error("unable to allocate scene composite descriptors");
  std::vector<VkDescriptorSet> atmosphere_sets(image_count);
  layouts.assign(image_count,atmosphere_descriptor_set_layout_);
  allocate.pSetLayouts=layouts.data();
  if(vkAllocateDescriptorSets(device_,&allocate,atmosphere_sets.data())!=VK_SUCCESS)
    throw std::runtime_error("unable to allocate atmosphere descriptors");
  for(std::size_t index=0;index<atmosphere_sets.size();++index)
    atmosphere_frames_[index].descriptor_set=atmosphere_sets[index];
  for(std::size_t index=0;index<descriptor_sets_.size();++index){
    VkDescriptorImageInfo image_info{
        shadow_sampler_,shadow_images_[index].view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo shadow_uniform_info{
        shadow_images_[index].uniform_buffer,0,
        sizeof(float)*(16U*shadow_map_layer_count+12U)};
    auto& frame=atmosphere_frames_[index];
    auto& atmosphere=atmosphere_lookups_;
    const std::array<VkDescriptorImageInfo,3> lighting_images{
        VkDescriptorImageInfo{scene_sampler_,atmosphere.transmittance.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.multiple_scattering.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.sky_irradiance.view,
                              VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo atmosphere_uniform_info{
        frame.uniform_buffer,0,sizeof(float)*96U};
    std::array<VkWriteDescriptorSet,6> shadow_writes{};
    shadow_writes[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    shadow_writes[0].dstSet=descriptor_sets_[index];
    shadow_writes[0].dstBinding=0;
    shadow_writes[0].descriptorCount=1;
    shadow_writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadow_writes[0].pImageInfo=&image_info;
    shadow_writes[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    shadow_writes[1].dstSet=descriptor_sets_[index];
    shadow_writes[1].dstBinding=1;
    shadow_writes[1].descriptorCount=1;
    shadow_writes[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadow_writes[1].pBufferInfo=&shadow_uniform_info;
    for(std::uint32_t binding=2;binding<4U;++binding){
      shadow_writes[binding].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      shadow_writes[binding].dstSet=descriptor_sets_[index];
      shadow_writes[binding].dstBinding=binding;
      shadow_writes[binding].descriptorCount=1;
      shadow_writes[binding].descriptorType=
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      shadow_writes[binding].pImageInfo=&lighting_images[binding-2U];
    }
    shadow_writes[4].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    shadow_writes[4].dstSet=descriptor_sets_[index];
    shadow_writes[4].dstBinding=4;
    shadow_writes[4].descriptorCount=1;
    shadow_writes[4].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadow_writes[4].pBufferInfo=&atmosphere_uniform_info;
    shadow_writes[5].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    shadow_writes[5].dstSet=descriptor_sets_[index];
    shadow_writes[5].dstBinding=5;
    shadow_writes[5].descriptorCount=1;
    shadow_writes[5].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadow_writes[5].pImageInfo=&lighting_images[2];
    vkUpdateDescriptorSets(device_,static_cast<std::uint32_t>(shadow_writes.size()),
                           shadow_writes.data(),0,nullptr);

    const std::array<VkDescriptorImageInfo,15> composite_images{
        VkDescriptorImageInfo{scene_sampler_,scene_colour_images_[index].view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{depth_sampler_,depth_images_[index].view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.transmittance.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.multiple_scattering.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.sky_view.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.aerial_scattering.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.aerial_transmittance.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{shadow_sampler_,shadow_images_[index].view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.sky_irradiance.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,atmosphere.long_shadow.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,frame.screen_scattering.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,frame.screen_transmittance.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{depth_sampler_,frame.screen_endpoint.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,frame.froxel_scattering.view,
                              VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{scene_sampler_,frame.froxel_transmittance.view,
                              VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo uniform_info{frame.uniform_buffer,0,
                                        sizeof(float)*96U};
    std::array<VkWriteDescriptorSet,17> composite_writes{};
    for(std::uint32_t binding=0;binding<composite_writes.size();++binding){
      auto& descriptor=composite_writes[binding];
      descriptor.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor.dstSet=composite_descriptor_sets_[index];
      descriptor.dstBinding=binding;
      descriptor.descriptorCount=1;
      if(binding==7U||binding==10U){
        descriptor.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptor.pBufferInfo=binding==7U?&uniform_info:&shadow_uniform_info;
      }else{
        const std::size_t image_index=binding<7U?binding:
            (binding==8U?7U:(binding==9U?8U:
             (binding==11U?9U:binding-2U)));
        descriptor.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor.pImageInfo=&composite_images[image_index];
      }
    }
    vkUpdateDescriptorSets(device_,
        static_cast<std::uint32_t>(composite_writes.size()),
        composite_writes.data(),0,nullptr);

    const std::array<VkImageView,7> storage_views{
        atmosphere.transmittance.view,atmosphere.multiple_scattering.view,
        atmosphere.sky_view.view,atmosphere.aerial_scattering.view,
        atmosphere.aerial_transmittance.view,
        atmosphere.sky_irradiance.view,atmosphere.long_shadow.view};
    std::array<VkDescriptorImageInfo,7> storage_images{};
    std::array<VkWriteDescriptorSet,26> atmosphere_writes{};
    for(std::uint32_t binding=0;binding<5U;++binding){
      storage_images[binding].imageView=storage_views[binding];
      storage_images[binding].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
      atmosphere_writes[binding].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      atmosphere_writes[binding].dstSet=frame.descriptor_set;
      atmosphere_writes[binding].dstBinding=binding;
      atmosphere_writes[binding].descriptorCount=1;
      atmosphere_writes[binding].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      atmosphere_writes[binding].pImageInfo=&storage_images[binding];
    }
    atmosphere_writes[5].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[5].dstSet=frame.descriptor_set;
    atmosphere_writes[5].dstBinding=5U;
    atmosphere_writes[5].descriptorCount=1;
    atmosphere_writes[5].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    atmosphere_writes[5].pBufferInfo=&uniform_info;
    atmosphere_writes[6].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[6].dstSet=frame.descriptor_set;
    atmosphere_writes[6].dstBinding=6U;
    atmosphere_writes[6].descriptorCount=1;
    atmosphere_writes[6].descriptorType=
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    atmosphere_writes[6].pImageInfo=&image_info;
    atmosphere_writes[7].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[7].dstSet=frame.descriptor_set;
    atmosphere_writes[7].dstBinding=7U;
    atmosphere_writes[7].descriptorCount=1;
    atmosphere_writes[7].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    atmosphere_writes[7].pBufferInfo=&shadow_uniform_info;
    storage_images[5].imageView=storage_views[5];
    storage_images[5].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    atmosphere_writes[8].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[8].dstSet=frame.descriptor_set;
    atmosphere_writes[8].dstBinding=8U;
    atmosphere_writes[8].descriptorCount=1;
    atmosphere_writes[8].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    atmosphere_writes[8].pImageInfo=&storage_images[5];
    VkDescriptorBufferInfo probe_info{frame.probe_buffer,0,
        sizeof(float)*4U*atmosphere_numeric_probe_value_count};
    atmosphere_writes[9].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[9].dstSet=frame.descriptor_set;
    atmosphere_writes[9].dstBinding=9U;
    atmosphere_writes[9].descriptorCount=1;
    atmosphere_writes[9].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    atmosphere_writes[9].pBufferInfo=&probe_info;
    storage_images[6].imageView=storage_views[6];
    storage_images[6].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    atmosphere_writes[10].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[10].dstSet=frame.descriptor_set;
    atmosphere_writes[10].dstBinding=10U;
    atmosphere_writes[10].descriptorCount=1;
    atmosphere_writes[10].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    atmosphere_writes[10].pImageInfo=&storage_images[6];
    VkDescriptorBufferInfo minmax_info{shadow_images_[index].minmax_buffer,0,
        VK_WHOLE_SIZE};
    atmosphere_writes[11].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[11].dstSet=frame.descriptor_set;
    atmosphere_writes[11].dstBinding=11U;
    atmosphere_writes[11].descriptorCount=1;
    atmosphere_writes[11].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    atmosphere_writes[11].pBufferInfo=&minmax_info;
    VkDescriptorImageInfo scene_depth_info{
        depth_sampler_,depth_images_[index].view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    atmosphere_writes[12].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    atmosphere_writes[12].dstSet=frame.descriptor_set;
    atmosphere_writes[12].dstBinding=12U;
    atmosphere_writes[12].descriptorCount=1;
    atmosphere_writes[12].descriptorType=
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    atmosphere_writes[12].pImageInfo=&scene_depth_info;
    const std::array<VkImageView,3> reconstructed_views{
        frame.screen_scattering.view,frame.screen_transmittance.view,
        frame.screen_endpoint.view};
    std::array<VkDescriptorImageInfo,3> reconstructed_storage{};
    for(std::uint32_t offset=0;offset<3U;++offset){
      reconstructed_storage[offset].imageView=reconstructed_views[offset];
      reconstructed_storage[offset].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
      auto& write=atmosphere_writes[13U+offset];
      write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet=frame.descriptor_set;
      write.dstBinding=13U+offset;
      write.descriptorCount=1;
      write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.pImageInfo=&reconstructed_storage[offset];
    }
    const std::array<VkImageView,6> history_views{
        frame.history_scattering[0].view,
        frame.history_transmittance[0].view,
        frame.history_endpoint[0].view,
        frame.history_scattering[1].view,
        frame.history_transmittance[1].view,
        frame.history_endpoint[1].view};
    std::array<VkDescriptorImageInfo,6> history_storage{};
    for(std::uint32_t offset=0;offset<history_storage.size();++offset){
      history_storage[offset].imageView=history_views[offset];
      history_storage[offset].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
      auto& write=atmosphere_writes[16U+offset];
      write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet=frame.descriptor_set;
      write.dstBinding=16U+offset;
      write.descriptorCount=1;
      write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.pImageInfo=&history_storage[offset];
    }
    const std::array<VkImageView,2> visibility_views{
        frame.history_visibility[0].view,frame.history_visibility[1].view};
    std::array<VkDescriptorImageInfo,2> visibility_storage{};
    for(std::uint32_t offset=0;offset<visibility_storage.size();++offset){
      visibility_storage[offset].imageView=visibility_views[offset];
      visibility_storage[offset].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
      auto& write=atmosphere_writes[22U+offset];
      write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet=frame.descriptor_set;
      write.dstBinding=22U+offset;
      write.descriptorCount=1;
      write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.pImageInfo=&visibility_storage[offset];
    }
    const std::array<VkImageView,2> froxel_views{
        frame.froxel_scattering.view,frame.froxel_transmittance.view};
    std::array<VkDescriptorImageInfo,2> froxel_storage{};
    for(std::uint32_t offset=0;offset<froxel_storage.size();++offset){
      froxel_storage[offset].imageView=froxel_views[offset];
      froxel_storage[offset].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
      auto& write=atmosphere_writes[24U+offset];
      write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet=frame.descriptor_set;
      write.dstBinding=24U+offset;
      write.descriptorCount=1;
      write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.pImageInfo=&froxel_storage[offset];
    }
    vkUpdateDescriptorSets(device_,
        static_cast<std::uint32_t>(atmosphere_writes.size()),
        atmosphere_writes.data(),0,nullptr);
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
  ++geometry_revision_;
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
  ++geometry_revision_;
}

void SceneRenderer::ensure_capture_resources(CaptureFrameResources& capture,
                                             VkExtent2D extent) {
  if(capture.image!=VK_NULL_HANDLE)return;
  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType=VK_IMAGE_TYPE_2D;
  image.format=colour_format_;
  image.extent={extent.width,extent.height,1U};
  image.mipLevels=1;
  image.arrayLayers=1;
  image.samples=VK_SAMPLE_COUNT_1_BIT;
  image.tiling=VK_IMAGE_TILING_OPTIMAL;
  image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if(vkCreateImage(device_,&image,nullptr,&capture.image)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene capture image");
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_,capture.image,&requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize=requirements.size;
  allocation.memoryTypeIndex=memory_type(
      physical_device_,requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(vkAllocateMemory(device_,&allocation,nullptr,
                      &capture.image_memory)!=VK_SUCCESS)
    throw std::runtime_error("unable to allocate scene capture image");
  if(vkBindImageMemory(device_,capture.image,capture.image_memory,0)!=VK_SUCCESS)
    throw std::runtime_error("unable to bind scene capture image");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image=capture.image;
  view.viewType=VK_IMAGE_VIEW_TYPE_2D;
  view.format=colour_format_;
  view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount=1;
  view.subresourceRange.layerCount=1;
  if(vkCreateImageView(device_,&view,nullptr,&capture.view)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene capture view");
  VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer.size=static_cast<VkDeviceSize>(extent.width)*extent.height*4U;
  buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if(vkCreateBuffer(device_,&buffer,nullptr,&capture.buffer)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene capture buffer");
  vkGetBufferMemoryRequirements(device_,capture.buffer,&requirements);
  allocation.allocationSize=requirements.size;
  allocation.memoryTypeIndex=memory_type(
      physical_device_,requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if(vkAllocateMemory(device_,&allocation,nullptr,
                      &capture.buffer_memory)!=VK_SUCCESS)
    throw std::runtime_error("unable to allocate scene capture buffer");
  if(vkBindBufferMemory(device_,capture.buffer,capture.buffer_memory,0)!=
     VK_SUCCESS)
    throw std::runtime_error("unable to bind scene capture buffer");
  if(vkCreateBuffer(device_,&buffer,nullptr,&capture.depth_buffer)!=VK_SUCCESS)
    throw std::runtime_error("unable to create scene depth capture buffer");
  vkGetBufferMemoryRequirements(device_,capture.depth_buffer,&requirements);
  allocation.allocationSize=requirements.size;
  allocation.memoryTypeIndex=memory_type(
      physical_device_,requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if(vkAllocateMemory(device_,&allocation,nullptr,
                      &capture.depth_buffer_memory)!=VK_SUCCESS)
    throw std::runtime_error("unable to allocate scene depth capture buffer");
  if(vkBindBufferMemory(device_,capture.depth_buffer,
                        capture.depth_buffer_memory,0)!=VK_SUCCESS)
    throw std::runtime_error("unable to bind scene depth capture buffer");
}

void SceneRenderer::record(VkCommandBuffer command_buffer,VkImageView colour_view,
                           std::uint32_t image_index,VkExtent2D extent,
                           const float* camera_data,
                           const AtmosphereFrameInput& atmosphere_input,
                           bool black_clear) {
  const auto begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
      vkGetDeviceProcAddr(device_, "vkCmdBeginRenderingKHR"));
  const auto end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
      vkGetDeviceProcAddr(device_, "vkCmdEndRenderingKHR"));
  if (begin_rendering == nullptr || end_rendering == nullptr)
    throw std::runtime_error("dynamic rendering is unavailable");

  constexpr std::uint32_t timing_count=8U;
  const std::uint32_t timing_base=image_index*timing_count;
  if(timing_queries_written_.at(image_index)){
    std::array<std::uint64_t,timing_count> timestamps{};
    if(vkGetQueryPoolResults(device_,timing_query_pool_,timing_base,timing_count,
        sizeof(timestamps),timestamps.data(),sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
      const auto elapsed=[&](std::size_t begin,std::size_t end){
        return static_cast<double>(timestamps[end]-timestamps[begin])*
            timestamp_period_nanoseconds_/1.0e6;
      };
      gpu_timings_={
          .shadows_milliseconds=elapsed(0,1),
          .atmosphere_milliseconds=elapsed(1,2),
          .terrain_milliseconds=elapsed(2,3),
          .depth_reduction_milliseconds=elapsed(3,4),
          .screen_integration_milliseconds=elapsed(4,5),
          .temporal_reconstruction_milliseconds=elapsed(5,6),
          .composite_milliseconds=elapsed(6,7),
          .valid=true};
    }
  }
  vkCmdResetQueryPool(command_buffer,timing_query_pool_,timing_base,timing_count);
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      timing_query_pool_,timing_base);
  timing_queries_written_[image_index]=true;

  auto& atmosphere_frame=atmosphere_frames_.at(image_index);
  auto& atmosphere=atmosphere_lookups_;
  if(atmosphere_frame.probe_pending){
    void* probe_mapped{};
    if(vkMapMemory(device_,atmosphere_frame.probe_memory,0,
          sizeof(float)*4U*atmosphere_numeric_probe_value_count,0,
          &probe_mapped)!=VK_SUCCESS)
      throw std::runtime_error("unable to map atmosphere probe buffer");
    std::memcpy(latest_atmosphere_probe_.values.data(),probe_mapped,
        sizeof(float)*4U*atmosphere_numeric_probe_value_count);
    vkUnmapMemory(device_,atmosphere_frame.probe_memory);
    latest_atmosphere_probe_.valid=true;
    atmosphere_frame.probe_pending=false;
  }
  auto& capture_frame=capture_frames_.at(image_index);
  if(capture_frame.pending){
    const std::size_t byte_count=static_cast<std::size_t>(extent.width)*
        extent.height*4U;
    latest_capture_.pixels.resize(byte_count);
    void* capture_mapped{};
    if(vkMapMemory(device_,capture_frame.buffer_memory,0,byte_count,0,
                   &capture_mapped)!=VK_SUCCESS)
      throw std::runtime_error("unable to map scene capture buffer");
    std::memcpy(latest_capture_.pixels.data(),capture_mapped,byte_count);
    vkUnmapMemory(device_,capture_frame.buffer_memory);
    latest_capture_.reversed_depth.resize(byte_count/sizeof(float));
    if(vkMapMemory(device_,capture_frame.depth_buffer_memory,0,byte_count,0,
                   &capture_mapped)!=VK_SUCCESS)
      throw std::runtime_error("unable to map scene depth capture buffer");
    std::memcpy(latest_capture_.reversed_depth.data(),capture_mapped,byte_count);
    vkUnmapMemory(device_,capture_frame.depth_buffer_memory);
    latest_capture_.width=extent.width;
    latest_capture_.height=extent.height;
    latest_capture_.bgra=colour_format_==VK_FORMAT_B8G8R8A8_UNORM||
                         colour_format_==VK_FORMAT_B8G8R8_UNORM;
    latest_capture_.valid=true;
    capture_frame.pending=false;
  }
  const auto& parameters=atmosphere_input.parameters;
  const bool atmosphere_valid=!validate_atmosphere(parameters).has_value();
  const bool atmosphere_enabled=atmosphere_input.enabled&&atmosphere_valid&&
                                !black_clear;
  const double metres=parameters.metres_per_world_unit;
  const auto physical_camera_from_centre=
      (atmosphere_input.camera_relative_world-
       atmosphere_input.planet_centre_relative_world)*metres;
  // Valleys legitimately descend below the smooth planetary datum.  Feeding
  // that physical position to the atmosphere model places the observer
  // inside its opaque reference sphere and makes the entire sky disappear.
  // Clamp the transport observer, not the rendered camera or terrain.
  const auto camera_from_centre=clamp_atmosphere_camera_to_medium(
      physical_camera_from_centre,parameters);
  const double camera_radius=std::sqrt(
      camera_from_centre.x*camera_from_centre.x+
      camera_from_centre.y*camera_from_centre.y+
      camera_from_centre.z*camera_from_centre.z);
  // The reference Hillaire camera volume must contain the complete grazing
  // atmosphere path.  The production local-volume cap is only 64 km, shorter
  // than this compact planet's roughly 130 km tangent path, so using it makes
  // far opaque pixels converge to a truncated integral while adjacent clear
  // pixels use the full sky integral.
  const double local_aerial_distance=
      atmosphere_input.transport==AtmosphereTransport::reference_hillaire_2020?
      atmosphere_input.maximum_aerial_distance_metres:
      atmosphere_local_aerial_distance(parameters,
          camera_radius-parameters.ground_radius_metres,
          atmosphere_input.maximum_aerial_distance_metres);
  std::array<float,96> atmosphere_uniform{};
  const auto spectrum=[&](std::size_t offset,
                          const AtmosphereSpectrum& value,float fourth){
    atmosphere_uniform[offset]=static_cast<float>(value[0]);
    atmosphere_uniform[offset+1U]=static_cast<float>(value[1]);
    atmosphere_uniform[offset+2U]=static_cast<float>(value[2]);
    atmosphere_uniform[offset+3U]=fourth;
  };
  spectrum(0U,parameters.rayleigh_scattering_per_metre,
           static_cast<float>(parameters.ground_radius_metres));
  spectrum(4U,parameters.mie_scattering_per_metre,
           static_cast<float>(parameters.ground_radius_metres+
                              parameters.atmosphere_height_metres));
  spectrum(8U,parameters.mie_absorption_per_metre,
           static_cast<float>(parameters.rayleigh_scale_height_metres));
  spectrum(12U,parameters.absorption_per_metre,
           static_cast<float>(parameters.mie_scale_height_metres));
  spectrum(16U,parameters.ground_albedo,
           static_cast<float>(parameters.mie_anisotropy));
  spectrum(20U,parameters.solar_irradiance,
           static_cast<float>(parameters.absorption_peak_altitude_metres));
  atmosphere_uniform[24]=static_cast<float>(
      parameters.absorption_half_width_metres);
  atmosphere_uniform[25]=static_cast<float>(metres);
  atmosphere_uniform[26]=static_cast<float>(
      parameters.solar_angular_radius_radians);
  atmosphere_uniform[27]=atmosphere_enabled?1.0F:0.0F;
  atmosphere_uniform[28]=static_cast<float>(camera_from_centre.x);
  atmosphere_uniform[29]=static_cast<float>(camera_from_centre.y);
  atmosphere_uniform[30]=static_cast<float>(camera_from_centre.z);
  atmosphere_uniform[31]=static_cast<float>(default_camera_near_plane*metres);
  atmosphere_uniform[32]=static_cast<float>(atmosphere_input.camera_right.x);
  atmosphere_uniform[33]=static_cast<float>(atmosphere_input.camera_right.y);
  atmosphere_uniform[34]=static_cast<float>(atmosphere_input.camera_right.z);
  atmosphere_uniform[35]=static_cast<float>(
      atmosphere_input.vertical_tangent*atmosphere_input.aspect_ratio);
  atmosphere_uniform[36]=static_cast<float>(atmosphere_input.camera_down.x);
  atmosphere_uniform[37]=static_cast<float>(atmosphere_input.camera_down.y);
  atmosphere_uniform[38]=static_cast<float>(atmosphere_input.camera_down.z);
  atmosphere_uniform[39]=static_cast<float>(atmosphere_input.vertical_tangent);
  atmosphere_uniform[40]=static_cast<float>(atmosphere_input.camera_forward.x);
  atmosphere_uniform[41]=static_cast<float>(atmosphere_input.camera_forward.y);
  atmosphere_uniform[42]=static_cast<float>(atmosphere_input.camera_forward.z);
  atmosphere_uniform[43]=static_cast<float>(
      local_aerial_distance);
  atmosphere_uniform[44]=static_cast<float>(atmosphere_input.sun_direction.x);
  atmosphere_uniform[45]=static_cast<float>(atmosphere_input.sun_direction.y);
  atmosphere_uniform[46]=static_cast<float>(atmosphere_input.sun_direction.z);
  atmosphere_uniform[47]=atmosphere_input.exposure;
  atmosphere_uniform[48]=static_cast<float>(
      atmosphere_input.camera_relative_world.x);
  atmosphere_uniform[49]=static_cast<float>(
      atmosphere_input.camera_relative_world.y);
  atmosphere_uniform[50]=static_cast<float>(
      atmosphere_input.camera_relative_world.z);
  atmosphere_uniform[51]=static_cast<float>(
      atmosphere_input.minimum_analytic_ground_distance_metres);
  atmosphere_uniform[52]=static_cast<float>(atmosphere_input.debug_view);
  atmosphere_uniform[53]=atmosphere_input.transport==
      AtmosphereTransport::qualified_baseline?0.0F:
      (atmosphere_input.transport==
           AtmosphereTransport::reference_hillaire_2020?10.0F:0.0F)+
      1.0F+static_cast<float>(atmosphere_input.rendering_method);
  const int shadow_filter=atmosphere_input.shadow_filter==
      AtmosphereShadowFilter::unfiltered?0:
      atmosphere_input.shadow_filter==AtmosphereShadowFilter::fixed_tent?1:2;
  int dynamic_shadow_phase=-1;
  if(atmosphere_input.dynamic_sun){
    if(dynamic_sun_was_active_){
      dynamic_shadow_phase=static_cast<int>(dynamic_sun_shadow_phase_);
      dynamic_sun_shadow_phase_=(dynamic_sun_shadow_phase_+1U)%4U;
    }else{
      dynamic_sun_was_active_=true;
      dynamic_sun_shadow_phase_=0U;
    }
  }else{
    dynamic_sun_was_active_=false;
    dynamic_sun_shadow_phase_=0U;
  }
  atmosphere_uniform[54]=static_cast<float>(shadow_filter+
      (atmosphere_input.numeric_probe_requested?10:0)+
      (atmosphere_input.dynamic_sun?100:0)+
      (dynamic_shadow_phase+1)*1000);
  atmosphere_uniform[55]=static_cast<float>(
      atmosphere_shadow_integrator_shader_index(
          atmosphere_input.shadow_integrator));
  const double inverse_camera_radius=1.0/std::max(camera_radius,1.0e-12);
  const tetra::Vec3 local_up{
      camera_from_centre.x*inverse_camera_radius,
      camera_from_centre.y*inverse_camera_radius,
      camera_from_centre.z*inverse_camera_radius};
  const double sun_projection=
      atmosphere_input.sun_direction.x*local_up.x+
      atmosphere_input.sun_direction.y*local_up.y+
      atmosphere_input.sun_direction.z*local_up.z;
  tetra::Vec3 sun_tangent{
      atmosphere_input.sun_direction.x-local_up.x*sun_projection,
      atmosphere_input.sun_direction.y-local_up.y*sun_projection,
      atmosphere_input.sun_direction.z-local_up.z*sun_projection};
  double tangent_length=std::sqrt(
      sun_tangent.x*sun_tangent.x+sun_tangent.y*sun_tangent.y+
      sun_tangent.z*sun_tangent.z);
  if(tangent_length<1.0e-5){
    const tetra::Vec3 reference=std::abs(local_up.z)<0.9?
        tetra::Vec3{0.0,0.0,1.0}:tetra::Vec3{1.0,0.0,0.0};
    const double projection=reference.x*local_up.x+
        reference.y*local_up.y+reference.z*local_up.z;
    sun_tangent={reference.x-local_up.x*projection,
                 reference.y-local_up.y*projection,
                 reference.z-local_up.z*projection};
    tangent_length=std::sqrt(
        sun_tangent.x*sun_tangent.x+sun_tangent.y*sun_tangent.y+
        sun_tangent.z*sun_tangent.z);
  }
  const double inverse_tangent_length=1.0/std::max(tangent_length,1.0e-12);
  atmosphere_uniform[56]=static_cast<float>(local_up.x);
  atmosphere_uniform[57]=static_cast<float>(local_up.y);
  atmosphere_uniform[58]=static_cast<float>(local_up.z);
  atmosphere_uniform[59]=static_cast<float>(camera_radius-
      parameters.ground_radius_metres);
  atmosphere_uniform[60]=static_cast<float>(
      sun_tangent.x*inverse_tangent_length);
  atmosphere_uniform[61]=static_cast<float>(
      sun_tangent.y*inverse_tangent_length);
  atmosphere_uniform[62]=static_cast<float>(
      sun_tangent.z*inverse_tangent_length);
  atmosphere_uniform[63]=static_cast<float>(std::sqrt(
      parameters.atmosphere_height_metres*
      (2.0*parameters.ground_radius_metres+
       parameters.atmosphere_height_metres)));
  void* mapped{};
  if(vkMapMemory(device_,atmosphere_frame.uniform_memory,0,
                 sizeof(atmosphere_uniform),0,&mapped)!=VK_SUCCESS)
    throw std::runtime_error("unable to map atmosphere uniform buffer");
  std::memcpy(mapped,atmosphere_uniform.data(),sizeof(atmosphere_uniform));
  vkUnmapMemory(device_,atmosphere_frame.uniform_memory);

  auto& shadow=shadow_images_.at(image_index);
  // FrameRender waits this swapchain image's fence before record() is called,
  // so a pending raster generation recorded on its previous use is now known
  // complete. Publish only at this fence-backed point, never while merely
  // recording the depth pass.
  if(shadow.atmosphere_shadow_pending_completion){
    atmosphere_shadow_map_status_.revision=std::max(
        atmosphere_shadow_map_status_.revision,
        shadow.atmosphere_shadow_pending_generation);
    atmosphere_shadow_map_status_.complete=
        shadow.atmosphere_shadow_pending_complete;
    shadow.atmosphere_shadow_pending_completion=false;
  }
  if(shadow.epipolar_diagnostic_pending){
    std::array<std::uint32_t,4> counters{};
    void* diagnostic_mapped{};
    if(vkMapMemory(device_,shadow.epipolar_diagnostic_memory,0,
                   sizeof(counters),0,&diagnostic_mapped)!=VK_SUCCESS)
      throw std::runtime_error("unable to map epipolar diagnostics");
    std::memcpy(counters.data(),diagnostic_mapped,sizeof(counters));
    vkUnmapMemory(device_,shadow.epipolar_diagnostic_memory);
    atmosphere_shadow_map_status_.epipolar_visited_nodes=counters[0];
    atmosphere_shadow_map_status_.epipolar_emitted_intervals=counters[1];
    atmosphere_shadow_map_status_.epipolar_fallbacks=counters[2];
    atmosphere_shadow_map_status_.epipolar_overflows=counters[3];
    shadow.epipolar_diagnostic_pending=false;
  }
  const auto cascades=make_stable_shadow_cascades(
      atmosphere_input.camera_relative_world,atmosphere_input.camera_forward,
      atmosphere_input.sun_direction,quality_settings_.shadow_resolution);
  const double local_comparison_bias_world=
      atmosphere_input.atmosphere_shadow_comparison_bias_world_override>=0.0?
      atmosphere_input.atmosphere_shadow_comparison_bias_world_override:0.0036;
  for(std::size_t cascade=0;cascade<shadow_cascade_count;++cascade){
    atmosphere_shadow_map_status_.local_depth_world_spans[cascade]=
        2.0*cascades.cascades[cascade].depth_half_range;
    atmosphere_shadow_map_status_.local_texel_world_sizes[cascade]=
        cascades.cascades[cascade].texel_world_size;
    atmosphere_shadow_map_status_.local_comparison_biases_normalized[cascade]=
        normalized_shadow_depth_bias(cascades.cascades[cascade],
                                     local_comparison_bias_world);
    atmosphere_shadow_map_status_.local_comparison_biases_world[cascade]=
        atmosphere_shadow_map_status_.local_comparison_biases_normalized[cascade]*
        atmosphere_shadow_map_status_.local_depth_world_spans[cascade];
  }
  if(atmosphere_input.numeric_probe_requested&&
     atmosphere_input.shadow_projection_probe_requested)
    throw std::invalid_argument("atmosphere probe modes are mutually exclusive");
  if(atmosphere_input.shadow_projection_probe_requested){
    std::array<std::array<float,4>,atmosphere_numeric_probe_value_count>
        probe_seed{};
    const auto cases=make_atmosphere_shadow_projection_probe_cases(cascades);
    for(std::size_t index=0;index<cases.size();++index){
      probe_seed[index]={static_cast<float>(cases[index].point.x),
                         static_cast<float>(cases[index].point.y),
                         static_cast<float>(cases[index].point.z),
                         static_cast<float>(cases[index].cascade)};
    }
    void* probe_mapped{};
    if(vkMapMemory(device_,atmosphere_frame.probe_memory,0,
          sizeof(probe_seed),0,&probe_mapped)!=VK_SUCCESS)
      throw std::runtime_error("unable to seed shadow projection probe");
    std::memcpy(probe_mapped,probe_seed.data(),sizeof(probe_seed));
    vkUnmapMemory(device_,atmosphere_frame.probe_memory);
  }
  const double receiver_distance_world=std::clamp(
      local_aerial_distance/std::max(metres,1.0e-12),
      cascades.cascades.back().split_distance,2048.0);
  const auto live_atmosphere_shadow_request=make_atmosphere_shadow_front_request(
      atmosphere_input.camera_relative_world,atmosphere_input.camera_forward,
      atmosphere_input.camera_right,atmosphere_input.camera_down,
      atmosphere_input.vertical_tangent,atmosphere_input.aspect_ratio,
      receiver_distance_world,1.15,atmosphere_input.sun_direction,
      receiver_distance_world,{},1U);
  const bool planned_front_complete=atmosphere_input.shadow_front&&
      atmosphere_input.shadow_front->complete();
  // While the background caster-residency front is still converging, render
  // a coherent fitted preview from the surface that is already published.
  // Disabling the fitted layer during this very visible startup interval
  // leaves low-sun receivers at the mercy of shallow local cascade depth and
  // makes distant mountains leak direct light across the foreground.
  const bool live_published_front=!planned_front_complete&&
      triangles_.count!=0U;
  const auto& atmosphere_shadow_request=planned_front_complete?
      atmosphere_input.shadow_front->request:live_atmosphere_shadow_request;
  auto atmosphere_shadow_fit=fit_atmosphere_shadow_map(
      atmosphere_shadow_request,
      quality_settings_.atmosphere_shadow_resolution);
  double fitted_receiver_distance=receiver_distance_world;
  // A replacement request is not consumable until its complete terrain front
  // arrives. Keep both the matrix and reach belonging to the previous depth
  // image; mixing a new matrix with old depth is worse than retaining an old
  // but internally coherent shadow generation.
  if(!planned_front_complete&&!live_published_front&&
     shadow.atmosphere_shadow_initialized){
    atmosphere_shadow_fit.matrix=shadow.atmosphere_shadow_matrix;
    fitted_receiver_distance=shadow.atmosphere_shadow_receiver_distance;
  }
  std::array<float,16U*shadow_map_layer_count+12U> shadow_uniform{};
  for(std::size_t cascade=0;cascade<shadow_cascade_count;++cascade){
    std::copy(cascades.cascades[cascade].matrix.begin(),
              cascades.cascades[cascade].matrix.end(),
              shadow_uniform.begin()+cascade*16U);
    shadow_uniform[16U*shadow_map_layer_count+cascade]=static_cast<float>(
        cascades.cascades[cascade].split_distance);
  }
  std::copy(atmosphere_shadow_fit.matrix.begin(),
            atmosphere_shadow_fit.matrix.end(),
            shadow_uniform.begin()+16U*shadow_cascade_count);
  const std::uint64_t planned_front_generation=planned_front_complete?
      atmosphere_input.shadow_front->generation:0U;
  // Fitted depth and its min/max hierarchy are sampleable only while their
  // planned front covers this frame. Local camera cascades remain current and
  // provide a conservative unshadowed fallback until the replacement lands.
  shadow_uniform[16U*shadow_map_layer_count+4U]=
      (planned_front_complete||live_published_front)&&
          shadow.atmosphere_shadow_initialized?1.0F:0.0F;
  shadow_uniform[16U*shadow_map_layer_count+5U]=
      static_cast<float>(fitted_receiver_distance);
  const double fitted_comparison_bias=
      atmosphere_input.atmosphere_shadow_comparison_bias_world_override>=0.0?
      atmosphere_input.atmosphere_shadow_comparison_bias_world_override/
          std::max(atmosphere_shadow_fit.depth_world_span,1.0e-12):
      atmosphere_fitted_shadow_depth_bias(
          atmosphere_shadow_fit.depth_world_span,
          atmosphere_shadow_fit.texel_world_size_x,
          atmosphere_shadow_fit.texel_world_size_y);
  shadow_uniform[16U*shadow_map_layer_count+6U]=
      static_cast<float>(fitted_comparison_bias);
  shadow_uniform[16U*shadow_map_layer_count+7U]=
      static_cast<float>(quality_settings_.atmosphere_shadow_resolution)/
      static_cast<float>(quality_settings_.shadow_resolution);
  const auto epipolar_layout=atmosphere_epipolar_layout(
      quality_settings_.atmosphere_shadow_resolution);
  shadow_uniform[16U*shadow_map_layer_count+8U]=
      static_cast<float>(epipolar_layout.radial_resolution);
  shadow_uniform[16U*shadow_map_layer_count+9U]=
      static_cast<float>(epipolar_layout.angular_rows);
  shadow_uniform[16U*shadow_map_layer_count+10U]=
      static_cast<float>(local_comparison_bias_world);
  shadow_uniform[16U*shadow_map_layer_count+11U]=
      static_cast<float>(shadow_minmax_element_count(
          quality_settings_.atmosphere_shadow_resolution));
  atmosphere_shadow_map_status_.epipolar_radial_resolution=
      epipolar_layout.radial_resolution;
  atmosphere_shadow_map_status_.epipolar_angular_rows=
      epipolar_layout.angular_rows;
  atmosphere_shadow_map_status_.epipolar_elements=
      epipolar_layout.element_count;
  atmosphere_shadow_map_status_.fitted_depth_world_span=
      atmosphere_shadow_fit.depth_world_span;
  atmosphere_shadow_map_status_.fitted_texel_world_size_x=
      atmosphere_shadow_fit.texel_world_size_x;
  atmosphere_shadow_map_status_.fitted_texel_world_size_y=
      atmosphere_shadow_fit.texel_world_size_y;
  atmosphere_shadow_map_status_.comparison_bias_normalized=
      fitted_comparison_bias;
  atmosphere_shadow_map_status_.comparison_bias_world=
      fitted_comparison_bias*atmosphere_shadow_fit.depth_world_span;
  if(vkMapMemory(device_,shadow.uniform_memory,0,sizeof(shadow_uniform),0,
                 &mapped)!=VK_SUCCESS)
    throw std::runtime_error("unable to map shadow cascade uniforms");
  std::memcpy(mapped,shadow_uniform.data(),sizeof(shadow_uniform));
  vkUnmapMemory(device_,shadow.uniform_memory);
  VkImageMemoryBarrier to_shadow{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_shadow.srcAccessMask=shadow.initialized?VK_ACCESS_SHADER_READ_BIT:0U;
  to_shadow.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_shadow.oldLayout=shadow.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      VK_IMAGE_LAYOUT_UNDEFINED;
  to_shadow.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_shadow.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_shadow.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_shadow.image=shadow.image;
  to_shadow.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
  to_shadow.subresourceRange.levelCount=1;
  to_shadow.subresourceRange.layerCount=static_cast<std::uint32_t>(
      shadow_map_layer_count);
  vkCmdPipelineBarrier(command_buffer,
      shadow.initialized?(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT):
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,0,0,nullptr,0,nullptr,1,
      &to_shadow);

  VkClearValue shadow_clear{};
  shadow_clear.depthStencil={depth_clear(shadow_map_depth_convention),0U};
  VkViewport shadow_viewport{0.0F,0.0F,
      static_cast<float>(quality_settings_.shadow_resolution),
      static_cast<float>(quality_settings_.shadow_resolution),0.0F,1.0F};
  VkRect2D shadow_scissor{{0,0},
      {quality_settings_.shadow_resolution,
       quality_settings_.shadow_resolution}};
  bool fitted_shadow_updated=false;
  for(std::size_t cascade=0;cascade<shadow_map_layer_count;++cascade){
    const bool fitted_layer=cascade==shadow_cascade_count;
    const bool update_fitted=!shadow.atmosphere_shadow_initialized||
        ((planned_front_complete||live_published_front)&&(
         shadow.atmosphere_shadow_matrix!=atmosphere_shadow_fit.matrix||
         shadow.atmosphere_shadow_front_generation!=planned_front_generation||
         shadow.atmosphere_surface_generation!=
             surface_upload_planner_.published_generation()));
    // Do not publish a complete-but-empty fitted depth generation during the
    // black startup frames.  Terrain upload and shadow-front planning are
    // independent; the depth image becomes initialized only once it contains
    // the published surface that the front describes.
    if(fitted_layer&&triangles_.count==0U)continue;
    if(fitted_layer&&!update_fitted)continue;
    const std::uint32_t layer_resolution=cascade<shadow_cascade_count?
        quality_settings_.shadow_resolution:
        quality_settings_.atmosphere_shadow_resolution;
    shadow_viewport.width=static_cast<float>(layer_resolution);
    shadow_viewport.height=static_cast<float>(layer_resolution);
    shadow_scissor.extent={layer_resolution,layer_resolution};
    VkRenderingAttachmentInfo shadow_attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    shadow_attachment.imageView=shadow.layer_views[cascade];
    shadow_attachment.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    shadow_attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadow_attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    shadow_attachment.clearValue=shadow_clear;
    VkRenderingInfo shadow_rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    shadow_rendering.renderArea.extent={layer_resolution,layer_resolution};
    shadow_rendering.layerCount=1;
    shadow_rendering.pDepthAttachment=&shadow_attachment;
    begin_rendering(command_buffer,
        reinterpret_cast<const VkRenderingInfoKHR*>(&shadow_rendering));
    vkCmdSetViewport(command_buffer,0,1,&shadow_viewport);
    vkCmdSetScissor(command_buffer,0,1,&shadow_scissor);
    vkCmdSetDepthBias(command_buffer,
        atmosphere_input.shadow_raster_bias_constant,0.0F,
        atmosphere_input.shadow_raster_bias_slope);
    atmosphere_shadow_map_status_.raster_bias_constant=
        atmosphere_input.shadow_raster_bias_constant;
    atmosphere_shadow_map_status_.raster_bias_slope=
        atmosphere_input.shadow_raster_bias_slope;
    if(fitted_layer)atmosphere_shadow_map_status_.caster_draws=0U;
    if(triangles_.count!=0U){
      VkDeviceSize shadow_offset{};
      vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadow_pipeline_);
      std::array<float,28> shadow_push{};
      const auto& shadow_matrix=cascade<shadow_cascade_count?
          cascades.cascades[cascade].matrix:atmosphere_shadow_fit.matrix;
      std::copy(shadow_matrix.begin(),shadow_matrix.end(),shadow_push.begin());
      vkCmdPushConstants(command_buffer,pipeline_layout_,
                         VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                         0,sizeof(float)*28,
                         shadow_push.data());
      vkCmdBindVertexBuffers(command_buffer,0,1,&triangles_.buffer,
                             &shadow_offset);
      const auto ranges=surface_upload_planner_.published_draws();
      std::size_t fitted_draws{};
      if(ranges.empty()){
        vkCmdDraw(command_buffer,static_cast<std::uint32_t>(triangles_.count),1,
                  0,0);
        fitted_draws=triangles_.count!=0U?1U:0U;
      }else for(const auto range:ranges){
        if(cascade==shadow_cascade_count){
          tetra::Vec3 projected_min{
              std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::infinity()};
          tetra::Vec3 projected_max{
              -projected_min.x,-projected_min.y,-projected_min.z};
          for(const double x:{range.minimum.x,range.maximum.x})
            for(const double y:{range.minimum.y,range.maximum.y})
              for(const double z:{range.minimum.z,range.maximum.z}){
                const auto projected=transform_shadow_point(
                    atmosphere_shadow_fit.matrix,{x,y,z});
                projected_min.x=std::min(projected_min.x,projected.x);
                projected_min.y=std::min(projected_min.y,projected.y);
                projected_min.z=std::min(projected_min.z,projected.z);
                projected_max.x=std::max(projected_max.x,projected.x);
                projected_max.y=std::max(projected_max.y,projected.y);
                projected_max.z=std::max(projected_max.z,projected.z);
              }
          if(projected_min.x>1.0||projected_max.x<-1.0||
             projected_min.y>1.0||projected_max.y<-1.0||
             projected_min.z>1.0||projected_max.z<0.0)
            continue;
        }
        vkCmdDraw(command_buffer,static_cast<std::uint32_t>(range.vertex_count),
                  1,static_cast<std::uint32_t>(range.first_vertex),0);
        ++fitted_draws;
      }
      if(fitted_layer)atmosphere_shadow_map_status_.caster_draws=fitted_draws;
    }
    end_rendering(command_buffer);
    if(fitted_layer){
      fitted_shadow_updated=true;
      ++shadow.atmosphere_shadow_depth_generation;
      atmosphere_shadow_map_status_.depth_generation=
          shadow.atmosphere_shadow_depth_generation;
      shadow.atmosphere_shadow_matrix=atmosphere_shadow_fit.matrix;
      shadow.atmosphere_surface_generation=
          surface_upload_planner_.published_generation();
      shadow.atmosphere_shadow_front_generation=planned_front_generation;
      shadow.atmosphere_shadow_receiver_distance=receiver_distance_world;
      shadow.atmosphere_shadow_initialized=true;
      ++atmosphere_shadow_map_status_.refreshes;
      shadow.atmosphere_shadow_pending_generation=planned_front_generation;
      shadow.atmosphere_shadow_pending_complete=planned_front_complete||
          planned_front_generation==0U;
      shadow.atmosphere_shadow_pending_completion=true;
    }
  }

  VkImageMemoryBarrier to_sample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_sample.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_sample.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  to_sample.oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_sample.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_sample.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_sample.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_sample.image=shadow.image;
  to_sample.subresourceRange=to_shadow.subresourceRange;
  vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,0,nullptr,0,nullptr,1,&to_sample);
  shadow.initialized=true;
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+1U);

  const std::array<VkImage,7> atmosphere_images{
      atmosphere.transmittance.image,atmosphere.multiple_scattering.image,
      atmosphere.sky_view.image,atmosphere.aerial_scattering.image,
      atmosphere.aerial_transmittance.image,atmosphere.sky_irradiance.image,
      atmosphere.long_shadow.image};
  if(!atmosphere.images_initialized){
    std::array<VkImageMemoryBarrier,7> initialize_barriers{};
    for(std::size_t index=0;index<initialize_barriers.size();++index){
      auto& barrier=initialize_barriers[index];
      barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=atmosphere_images[index];
      barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount=1;
      barrier.subresourceRange.layerCount=1;
    }
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
        static_cast<std::uint32_t>(initialize_barriers.size()),
        initialize_barriers.data());
    atmosphere.images_initialized=true;
  }else if(atmosphere_enabled){
    // The lookup images are shared by all swapchain frames. A moving camera
    // can make this frame regenerate aerial/sky data while the preceding
    // frame is still sampling those same images in tone mapping. Submission
    // order alone is not a memory dependency: close the cross-frame
    // fragment-read -> compute-write hazard before any lookup dispatch.
    std::array<VkImageMemoryBarrier,7> reuse_barriers{};
    for(std::size_t index=0;index<reuse_barriers.size();++index){
      auto& barrier=reuse_barriers[index];
      barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;
      barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|
          VK_ACCESS_SHADER_WRITE_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=atmosphere_images[index];
      barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount=1;
      barrier.subresourceRange.layerCount=1;
    }
    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
        static_cast<std::uint32_t>(reuse_barriers.size()),
        reuse_barriers.data());
  }
  // Descriptor validation is based on every image a shader can access, not
  // only the branch selected at runtime. Establish GENERAL for the complete
  // per-frame descriptor image set before any atmosphere dispatch or draw.
  if(!atmosphere_frame.descriptor_images_initialized){
    const std::array<VkImage,13> descriptor_images{
        atmosphere_frame.screen_scattering.image,
        atmosphere_frame.screen_transmittance.image,
        atmosphere_frame.screen_endpoint.image,
        atmosphere_frame.froxel_scattering.image,
        atmosphere_frame.froxel_transmittance.image,
        atmosphere_frame.history_scattering[0].image,
        atmosphere_frame.history_transmittance[0].image,
        atmosphere_frame.history_endpoint[0].image,
        atmosphere_frame.history_visibility[0].image,
        atmosphere_frame.history_scattering[1].image,
        atmosphere_frame.history_transmittance[1].image,
        atmosphere_frame.history_endpoint[1].image,
        atmosphere_frame.history_visibility[1].image};
    std::array<VkImageMemoryBarrier,descriptor_images.size()> barriers{};
    for(std::size_t index=0;index<barriers.size();++index){
      auto& barrier=barriers[index];
      barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=descriptor_images[index];
      barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount=1U;
      barrier.subresourceRange.layerCount=1U;
    }
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,nullptr,0,nullptr,static_cast<std::uint32_t>(barriers.size()),
        barriers.data());
    atmosphere_frame.descriptor_images_initialized=true;
  }
  if(atmosphere_enabled){
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        atmosphere_input.transport==AtmosphereTransport::qualified_baseline?
            atmosphere_pipeline_:faithful_atmosphere_pipeline_);
    vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        atmosphere_pipeline_layout_,0,1,&atmosphere_frame.descriptor_set,0,
        nullptr);
    const bool temporal=atmosphere_input.transport==
        AtmosphereTransport::reference_hillaire_2020||
        atmosphere_input.rendering_method==
            AtmosphereRenderingMethod::temporal_half_resolution;
    if(temporal){
      const std::array<VkImage,8> history_images{
          atmosphere_frame.history_scattering[0].image,
          atmosphere_frame.history_transmittance[0].image,
          atmosphere_frame.history_endpoint[0].image,
          atmosphere_frame.history_visibility[0].image,
          atmosphere_frame.history_scattering[1].image,
          atmosphere_frame.history_transmittance[1].image,
          atmosphere_frame.history_endpoint[1].image,
          atmosphere_frame.history_visibility[1].image};
      std::array<VkImageMemoryBarrier,8> prepare{};
      for(std::size_t index=0;index<prepare.size();++index){
        auto& barrier=prepare[index];
        barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask=atmosphere_frame.history_images_initialized?
            (VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT):0U;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|
            VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=history_images[index];
        barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount=1U;
        barrier.subresourceRange.layerCount=1U;
      }
      vkCmdPipelineBarrier(command_buffer,
          atmosphere_frame.history_images_initialized?
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
          static_cast<std::uint32_t>(prepare.size()),prepare.data());
      atmosphere_frame.history_images_initialized=true;
    }
    const auto dispatch=[&](std::uint32_t mode,std::uint32_t x,
                            std::uint32_t y,std::uint32_t z){
      const std::array<std::uint32_t,4> push{mode,0U,0U,0U};
      vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
          VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());
      vkCmdDispatch(command_buffer,x,y,z);
      if(mode==0U)++atmosphere_dispatch_counts_.transmittance;
      else if(mode==1U)++atmosphere_dispatch_counts_.multiple_scattering;
      else if(mode==2U)++atmosphere_dispatch_counts_.sky_view;
      else if(mode==3U)++atmosphere_dispatch_counts_.aerial_perspective;
      else if(mode==4U)++atmosphere_dispatch_counts_.sky_irradiance;
      else if(mode==6U)++atmosphere_dispatch_counts_.long_shadow;
    };
    const auto compute_barrier=[&](std::span<const VkImage> images,
                                   VkPipelineStageFlags destination_stage){
      std::vector<VkImageMemoryBarrier> barriers(images.size());
      for(std::size_t index=0;index<images.size();++index){
        auto& barrier=barriers[index];
        barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|
            (destination_stage==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT?
                 VK_ACCESS_SHADER_WRITE_BIT:0U);
        barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=images[index];
        barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount=1;
        barrier.subresourceRange.layerCount=1;
      }
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          destination_stage,0,0,nullptr,0,nullptr,
          static_cast<std::uint32_t>(barriers.size()),barriers.data());
    };
    const bool hierarchy_integrator=atmosphere_input.shadow_integrator==
        AtmosphereShadowIntegrator::minmax_segments||
        atmosphere_input.shadow_integrator==
            AtmosphereShadowIntegrator::moment_hybrid||
        atmosphere_input.shadow_integrator==
            AtmosphereShadowIntegrator::epipolar_minmax;
    const bool epipolar_integrator=atmosphere_input.shadow_integrator==
        AtmosphereShadowIntegrator::epipolar_minmax;
    const auto epipolar_source_revision=atmosphere_epipolar_source_revision(
        surface_upload_planner_.published_generation(),
        cascades.cascades[3].matrix,
        atmosphere_input.shadow_raster_bias_constant,
        atmosphere_input.shadow_raster_bias_slope,
        atmosphere_input.shadow_filter,local_comparison_bias_world);
    if(hierarchy_integrator&&!epipolar_integrator&&
       (fitted_shadow_updated||shadow.minmax_generation!=
          shadow.atmosphere_shadow_depth_generation||
        (shadow.minmax_is_epipolar&&
         (!epipolar_integrator||shadow.epipolar_generation!=
           shadow.atmosphere_shadow_depth_generation)))){
      std::uint32_t level_size=
          quality_settings_.atmosphere_shadow_resolution;
      std::uint32_t level{};
      while(true){
        const std::array<std::uint32_t,4> push{
            8U,level,quality_settings_.atmosphere_shadow_resolution,
            epipolar_integrator?3U:4U};
        vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
            VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());
        vkCmdDispatch(command_buffer,(level_size+7U)/8U,
                      (level_size+7U)/8U,1U);
        VkBufferMemoryBarrier hierarchy_barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hierarchy_barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        hierarchy_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|
            VK_ACCESS_SHADER_WRITE_BIT;
        hierarchy_barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        hierarchy_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        hierarchy_barrier.buffer=shadow.minmax_buffer;
        hierarchy_barrier.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command_buffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,
            &hierarchy_barrier,0,nullptr);
        if(level_size==1U)break;
        level_size=(level_size+1U)/2U;
        ++level;
      }
      shadow.minmax_generation=shadow.atmosphere_shadow_depth_generation;
      shadow.minmax_is_epipolar=false;
      atmosphere_shadow_map_status_.hierarchy_generation=
          shadow.minmax_generation;
      atmosphere_shadow_map_status_.hierarchy_complete=true;
    }
    if(epipolar_integrator&&
       (shadow.epipolar_source_revision!=epipolar_source_revision||
        !shadow.minmax_is_epipolar)){
      const auto radial_resolution=static_cast<std::uint32_t>(
          epipolar_layout.radial_resolution);
      const auto angular_rows=static_cast<std::uint32_t>(
          epipolar_layout.angular_rows);
      const std::array<std::uint32_t,4> base_push{
          9U,quality_settings_.atmosphere_shadow_resolution,
          radial_resolution,angular_rows};
      vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
          VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(base_push),base_push.data());
      vkCmdDispatch(command_buffer,(radial_resolution+7U)/8U,
                    (angular_rows+7U)/8U,1U);
      VkBufferMemoryBarrier epipolar_barrier{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      epipolar_barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      epipolar_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|
          VK_ACCESS_SHADER_WRITE_BIT;
      epipolar_barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      epipolar_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      epipolar_barrier.buffer=shadow.minmax_buffer;
      epipolar_barrier.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,
          &epipolar_barrier,0,nullptr);
      std::uint32_t level=1U;
      std::uint32_t width=(radial_resolution+1U)/2U;
      while(radial_resolution>1U){
        const std::array<std::uint32_t,4> mip_push{
            10U,level,radial_resolution,angular_rows};
        vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
            VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(mip_push),mip_push.data());
        vkCmdDispatch(command_buffer,(width+7U)/8U,
                      (angular_rows+7U)/8U,1U);
        vkCmdPipelineBarrier(command_buffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,
            &epipolar_barrier,0,nullptr);
        if(width==1U)break;
        width=(width+1U)/2U;
        ++level;
      }
      shadow.epipolar_generation=shadow.atmosphere_shadow_depth_generation;
      shadow.epipolar_source_revision=epipolar_source_revision;
      shadow.minmax_is_epipolar=true;
      ++atmosphere_shadow_map_status_.epipolar_hierarchy_refreshes;
    }
    atmosphere_shadow_map_status_.integrator=
        atmosphere_input.shadow_integrator;
    const bool hierarchy_generation_complete=epipolar_integrator?
        shadow.epipolar_source_revision==epipolar_source_revision:
        shadow.minmax_generation==shadow.atmosphere_shadow_depth_generation;
    atmosphere_shadow_map_status_.hierarchy_complete=hierarchy_integrator&&
        shadow.atmosphere_shadow_depth_generation!=0U&&
        hierarchy_generation_complete&&
        (epipolar_integrator==shadow.minmax_is_epipolar);
    std::uint64_t atmosphere_shadow_revision=1469598103934665603ULL;
    hash_scalar(atmosphere_shadow_revision,static_cast<double>(
        surface_upload_planner_.published_generation()));
    hash_scalar(atmosphere_shadow_revision,static_cast<double>(
        shadow.atmosphere_shadow_depth_generation));
    hash_scalar(atmosphere_shadow_revision,
        shadow.atmosphere_shadow_initialized?1.0:0.0);
    for(const float value:shadow.atmosphere_shadow_matrix)
      hash_scalar(atmosphere_shadow_revision,static_cast<double>(value));
    const AtmosphereLookupRevisions next_revisions{
        .optical={atmosphere_optical_hash(parameters)},
        .scattering={atmosphere_scattering_hash(parameters)},
        // Faithful final sky and aerial pixels evaluate the current primary
        // ray directly. During a fast sun preview, keep the diagnostic and
        // indirect-light lookup generation stable instead of rebuilding the
        // complete 3D atmosphere volume every frame.
        .sun={atmosphere_input.dynamic_sun?0xd79a4f3b2c1e6805ULL:
              hash_vectors({atmosphere_input.sun_direction})},
        .camera_position={hash_vectors({camera_from_centre})},
        .sky_position=atmosphere_sky_position_revision(
            camera_from_centre,parameters),
        .camera_orientation={hash_vectors(
            {atmosphere_input.camera_right,atmosphere_input.camera_down,
             atmosphere_input.camera_forward},
            {atmosphere_input.vertical_tangent,atmosphere_input.aspect_ratio})},
        .shadow_integrator={static_cast<std::uint64_t>(
            atmosphere_input.shadow_integrator)+1U},
        .shadow={atmosphere_shadow_revision},
        .render_origin={hash_vectors(
            {atmosphere_input.planet_centre_relative_world})}};
    const auto previous_revisions=atmosphere.transport==
        atmosphere_input.transport?
        atmosphere.lookup_revisions:std::nullopt;
    auto plan=atmosphere_dispatch_plan(previous_revisions,next_revisions,
        atmosphere_input.transport);
    if(atmosphere_input.dynamic_sun&&previous_revisions)
      plan.aerial_perspective=false;
    if(plan.transmittance){
      dispatch(0U,(quality_settings_.transmittance_width+7U)/8U,
               (quality_settings_.transmittance_height+7U)/8U,1U);
      compute_barrier(std::span<const VkImage>{atmosphere_images.data(),1U},
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    if(plan.multiple_scattering){
      dispatch(1U,(quality_settings_.multiple_scattering_size+7U)/8U,
               (quality_settings_.multiple_scattering_size+7U)/8U,1U);
      compute_barrier(std::span<const VkImage>{atmosphere_images.data(),2U},
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    if(plan.sky_view){
      if(atmosphere_input.transport==
         AtmosphereTransport::reference_hillaire_2020)
        vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
                          reference_hillaire_atmosphere_pipeline_);
      dispatch(2U,(quality_settings_.sky_width+7U)/8U,
               (quality_settings_.sky_height+7U)/8U,1U);
      if(atmosphere_input.transport==
         AtmosphereTransport::reference_hillaire_2020)
        vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
                          faithful_atmosphere_pipeline_);
    }
    if(plan.sky_irradiance&&atmosphere_input.transport!=
       AtmosphereTransport::qualified_baseline){
      if(plan.sky_view)
        compute_barrier(
            std::span<const VkImage>{atmosphere_images.data()+2U,1U},
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      dispatch(4U,(quality_settings_.irradiance_width+7U)/8U,
               (quality_settings_.irradiance_height+7U)/8U,1U);
    }
    if(plan.aerial_perspective)
      dispatch(3U,(quality_settings_.aerial_width+7U)/8U,
               (quality_settings_.aerial_height+7U)/8U,
               quality_settings_.aerial_depth);
    if(plan.long_shadow&&epipolar_integrator){
      const std::array<std::uint32_t,4> reset_push{11U,0U,0U,0U};
      vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
          VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(reset_push),reset_push.data());
      vkCmdDispatch(command_buffer,1U,1U,1U);
      VkBufferMemoryBarrier reset_barrier{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      reset_barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      reset_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|
          VK_ACCESS_SHADER_WRITE_BIT;
      reset_barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      reset_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      reset_barrier.buffer=shadow.minmax_buffer;
      reset_barrier.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&reset_barrier,
          0,nullptr);
    }
    if(plan.long_shadow)
      dispatch(6U,(quality_settings_.long_shadow_width+7U)/8U,
               (quality_settings_.long_shadow_height+7U)/8U,1U);
    if(plan.long_shadow&&epipolar_integrator){
      VkBufferMemoryBarrier counter_to_copy{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      counter_to_copy.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      counter_to_copy.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      counter_to_copy.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      counter_to_copy.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      counter_to_copy.buffer=shadow.minmax_buffer;
      counter_to_copy.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&counter_to_copy,
          0,nullptr);
      const auto counter_offset=(shadow_minmax_element_count(
          quality_settings_.atmosphere_shadow_resolution)-2U)*
          sizeof(std::uint32_t)*2U;
      const VkBufferCopy copy{counter_offset,0,sizeof(std::uint32_t)*4U};
      vkCmdCopyBuffer(command_buffer,shadow.minmax_buffer,
                      shadow.epipolar_diagnostic_buffer,1U,&copy);
      VkBufferMemoryBarrier copy_to_host{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      copy_to_host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
      copy_to_host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      copy_to_host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      copy_to_host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      copy_to_host.buffer=shadow.epipolar_diagnostic_buffer;
      copy_to_host.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&copy_to_host,0,nullptr);
      shadow.epipolar_diagnostic_pending=true;
    }else if(!epipolar_integrator){
      atmosphere_shadow_map_status_.epipolar_visited_nodes=0U;
      atmosphere_shadow_map_status_.epipolar_emitted_intervals=0U;
      atmosphere_shadow_map_status_.epipolar_fallbacks=0U;
      atmosphere_shadow_map_status_.epipolar_overflows=0U;
    }
    if(atmosphere_input.numeric_probe_requested&&
       atmosphere_input.transport==AtmosphereTransport::faithful_hillaire){
      compute_barrier(atmosphere_images,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      dispatch(5U,1U,1U,1U);
      VkBufferMemoryBarrier probe_barrier{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      probe_barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      probe_barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      probe_barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      probe_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      probe_barrier.buffer=atmosphere_frame.probe_buffer;
      probe_barrier.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&probe_barrier,0,nullptr);
      atmosphere_frame.probe_pending=true;
    }
    if(atmosphere_input.shadow_projection_probe_requested){
      VkBufferMemoryBarrier upload_barrier{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      upload_barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
      upload_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
      upload_barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      upload_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      upload_barrier.buffer=atmosphere_frame.probe_buffer;
      upload_barrier.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_HOST_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&upload_barrier,
          0,nullptr);
      dispatch(7U,1U,1U,1U);
      VkBufferMemoryBarrier readback_barrier{
          VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      readback_barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      readback_barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      readback_barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      readback_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      readback_barrier.buffer=atmosphere_frame.probe_buffer;
      readback_barrier.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&readback_barrier,0,nullptr);
      atmosphere_frame.probe_pending=true;
    }
    compute_barrier(atmosphere_images,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const auto material=atmosphere_material_snapshot(
        parameters,atmosphere_input.transport);
    const auto snapshots=advance_atmosphere_lookup_snapshots(
        atmosphere.transport==atmosphere_input.transport?
            atmosphere.lookup_snapshots:std::nullopt,
        material,next_revisions,plan);
    const auto validation=atmosphere_validation_snapshot(
        material,snapshots,next_revisions);
    if(!validation.compatible())
      throw std::runtime_error("incompatible atmosphere lookup generation: "+
                               *validation.incompatibility);
    atmosphere.lookup_snapshots=snapshots;
    atmosphere.lookup_revisions=next_revisions;
    atmosphere.transport=atmosphere_input.transport;
    atmosphere.shadow_integrator=atmosphere_input.shadow_integrator;
  }
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+2U);

  auto& scene_colour=scene_colour_images_.at(image_index);
  auto& scene_depth=depth_images_.at(image_index);
  std::array<VkImageMemoryBarrier,4> to_scene{};
  to_scene[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_scene[0].srcAccessMask=scene_colour.initialized?VK_ACCESS_SHADER_READ_BIT:0U;
  to_scene[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  to_scene[0].oldLayout=scene_colour.initialized?
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
  to_scene[0].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_scene[0].srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_scene[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_scene[0].image=scene_colour.image;
  to_scene[0].subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
  to_scene[0].subresourceRange.levelCount=1;
  to_scene[0].subresourceRange.layerCount=1;
  to_scene[1]=to_scene[0];
  to_scene[1].srcAccessMask=scene_depth.initialized?VK_ACCESS_SHADER_READ_BIT:0U;
  to_scene[1].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_scene[1].oldLayout=scene_depth.initialized?
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
  to_scene[1].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_scene[1].image=scene_depth.image;
  to_scene[1].subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
  std::uint32_t to_scene_count=2U;
  if(terrain_msaa_enabled_){
    to_scene[2]=to_scene[0];
    to_scene[2].srcAccessMask=msaa_colour_image_.initialized?
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:0U;
    to_scene[2].oldLayout=msaa_colour_image_.initialized?
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    to_scene[2].image=msaa_colour_image_.image;
    to_scene[3]=to_scene[1];
    to_scene[3].srcAccessMask=msaa_depth_image_.initialized?
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:0U;
    to_scene[3].oldLayout=msaa_depth_image_.initialized?
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        VK_IMAGE_LAYOUT_UNDEFINED;
    to_scene[3].image=msaa_depth_image_.image;
    to_scene_count=4U;
  }
  VkPipelineStageFlags scene_source_stages=
      (scene_colour.initialized||scene_depth.initialized)?
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  if(terrain_msaa_enabled_&&msaa_colour_image_.initialized)
    scene_source_stages|=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  vkCmdPipelineBarrier(command_buffer,
      scene_source_stages,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      0,0,nullptr,0,nullptr,to_scene_count,
      to_scene.data());

  VkClearValue colour{};
  const float clear_alpha=terrain_msaa_enabled_&&atmosphere_enabled?0.0F:1.0F;
  colour.color=black_clear||clear_alpha==0.0F?
      VkClearColorValue{{0.0F,0.0F,0.0F,clear_alpha}}:
      VkClearColorValue{{0.06F,0.08F,0.11F,clear_alpha}};
  VkClearValue depth{};
  depth.depthStencil={depth_clear(main_scene_depth_convention),0U};
  VkRenderingAttachmentInfo colour_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; colour_attachment.imageView = terrain_msaa_enabled_?msaa_colour_image_.view:scene_colour.view; colour_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; colour_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colour_attachment.storeOp = terrain_msaa_enabled_?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE; colour_attachment.clearValue = colour;
  VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth_attachment.imageView = terrain_msaa_enabled_?msaa_depth_image_.view:scene_depth.view; depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth_attachment.storeOp = terrain_msaa_enabled_?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE; depth_attachment.clearValue = depth;
  if(terrain_msaa_enabled_){
    colour_attachment.resolveMode=VK_RESOLVE_MODE_AVERAGE_BIT;
    colour_attachment.resolveImageView=scene_colour.view;
    colour_attachment.resolveImageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // The scene uses reversed depth, so the closest covered sample has the
    // greatest value and must win the single-sample atmosphere depth.
    depth_attachment.resolveMode=VK_RESOLVE_MODE_MAX_BIT;
    depth_attachment.resolveImageView=scene_depth.view;
    depth_attachment.resolveImageLayout=
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = extent; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &colour_attachment; rendering.pDepthAttachment = &depth_attachment;
  begin_rendering(command_buffer, reinterpret_cast<const VkRenderingInfoKHR*>(&rendering));
  VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F}; VkRect2D scissor{{0, 0}, extent}; VkDeviceSize offset{};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport); vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  std::array<float,28> push_data{};
  std::copy_n(camera_data,28,push_data.begin());
  // The light vector's fourth component is intentionally not geometric. It
  // carries the independently selectable surface receiver-bias experiment.
  push_data[19]=atmosphere_input.surface_shadow_bias==
      SurfaceShadowBiasMode::receiver_plane?1.0F:0.0F;
  const auto draw = [&](VkPipeline pipeline,VkPipelineLayout layout,
                        const VertexBuffer& vertices,
                        std::span<const SurfaceDeviceDrawRange> ranges={}) {
    if (vertices.count == 0) return;
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertices.buffer, &offset);
    vkCmdPushConstants(command_buffer,layout,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,
        sizeof(float)*28,push_data.data());
    if(layout==shaded_pipeline_layout_)
      vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
          layout,0,1,&descriptor_sets_.at(image_index),0,nullptr);
    if(ranges.empty())
      vkCmdDraw(command_buffer,static_cast<std::uint32_t>(vertices.count),1,0,0);
    else for(const auto range:ranges)
      vkCmdDraw(command_buffer,static_cast<std::uint32_t>(range.vertex_count),1,
                static_cast<std::uint32_t>(range.first_vertex),0);
  };
  // Opaque geometry establishes visibility first. The native line-mode pass
  // reuses those triangle vertices and depths, so hidden rear edges fail the
  // depth test and visible edges do not depend on triangle shape.
  if(!black_clear&&!atmosphere_enabled){
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
        terrain_msaa_enabled_?msaa_sky_pipeline_:sky_pipeline_);
    vkCmdPushConstants(command_buffer,pipeline_layout_,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,
        sizeof(float)*28,push_data.data());
    vkCmdDraw(command_buffer,6U,1U,0U,0U);
  }
  draw(terrain_msaa_enabled_?msaa_triangle_pipeline_:triangle_pipeline_,
       shaded_pipeline_layout_,triangles_,
       surface_upload_planner_.published_draws());
  draw(terrain_msaa_enabled_?msaa_triangle_wire_pipeline_:
                               triangle_wire_pipeline_,
       pipeline_layout_,triangles_,
       surface_upload_planner_.published_draws());
  push_data[24]=static_cast<float>(extent.width);
  push_data[25]=static_cast<float>(extent.height);
  // One-pixel half-extent provides the complete filter footprint for an
  // analytically antialiased one-pixel centre line.
  push_data[26]=1.0F;
  push_data[27]=0.0F;
  draw(terrain_msaa_enabled_?msaa_line_pipeline_:line_pipeline_,
       pipeline_layout_,hierarchy_lines_);
  draw(terrain_msaa_enabled_?msaa_editor_line_pipeline_:
                               editor_line_pipeline_,
       pipeline_layout_,editor_lines_);
  end_rendering(command_buffer);
  if(terrain_msaa_enabled_){
    msaa_colour_image_.initialized=true;
    msaa_depth_image_.initialized=true;
  }
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+3U);

  std::array<VkImageMemoryBarrier,2> to_composite{};
  to_composite[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_composite[0].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  to_composite[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  to_composite[0].oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_composite[0].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_composite[0].srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_composite[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_composite[0].image=scene_colour.image;
  to_composite[0].subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
  to_composite[0].subresourceRange.levelCount=1;
  to_composite[0].subresourceRange.layerCount=1;
  to_composite[1]=to_composite[0];
  to_composite[1].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_composite[1].oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_composite[1].image=scene_depth.image;
  to_composite[1].subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
  vkCmdPipelineBarrier(command_buffer,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
      static_cast<std::uint32_t>(to_composite.size()),to_composite.data());
  scene_colour.initialized=true;
  scene_depth.initialized=true;

  if(atmosphere_enabled&&
     (atmosphere_input.transport==AtmosphereTransport::faithful_hillaire||
      atmosphere_input.transport==AtmosphereTransport::reference_hillaire_2020)&&
     atmosphere_input.rendering_method==
         AtmosphereRenderingMethod::deterministic_shadowed_froxels){
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timing_query_pool_,timing_base+4U);
    const std::array<VkImage,2> froxel_images{
        atmosphere_frame.froxel_scattering.image,
        atmosphere_frame.froxel_transmittance.image};
    std::array<VkImageMemoryBarrier,2> froxel_barriers{};
    for(std::size_t index=0;index<froxel_barriers.size();++index){
      auto& barrier=froxel_barriers[index];
      barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcAccessMask=atmosphere_frame.froxel_images_initialized?
          VK_ACCESS_SHADER_READ_BIT:0U;
      barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=froxel_images[index];
      barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount=1U;
      barrier.subresourceRange.layerCount=1U;
    }
    vkCmdPipelineBarrier(command_buffer,
        atmosphere_frame.froxel_images_initialized?
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
        static_cast<std::uint32_t>(froxel_barriers.size()),
        froxel_barriers.data());
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        atmosphere_input.transport==AtmosphereTransport::reference_hillaire_2020?
            reference_hillaire_atmosphere_pipeline_:
            faithful_atmosphere_pipeline_);
    vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        atmosphere_pipeline_layout_,0,1,&atmosphere_frame.descriptor_set,0,
        nullptr);
    const std::array<std::uint32_t,4> froxel_push{16U,0U,0U,0U};
    vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(froxel_push),froxel_push.data());
    vkCmdDispatch(command_buffer,4U,4U,32U);
    for(auto& barrier:froxel_barriers){
      barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,
        static_cast<std::uint32_t>(froxel_barriers.size()),
        froxel_barriers.data());
    atmosphere_frame.froxel_images_initialized=true;
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        timing_query_pool_,timing_base+5U);
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timing_query_pool_,timing_base+6U);
  }else if(atmosphere_enabled&&(
      atmosphere_input.transport==AtmosphereTransport::reference_hillaire_2020||
      (atmosphere_input.transport==AtmosphereTransport::faithful_hillaire&&
       (atmosphere_input.rendering_method==
            AtmosphereRenderingMethod::deterministic_half_resolution||
        atmosphere_input.rendering_method==
            AtmosphereRenderingMethod::temporal_half_resolution)))){
    const bool reference_hillaire=atmosphere_input.transport==
        AtmosphereTransport::reference_hillaire_2020;
    const bool temporal=reference_hillaire||
        atmosphere_input.rendering_method==
            AtmosphereRenderingMethod::temporal_half_resolution;
    const std::array<VkImage,3> reconstructed_images{
        atmosphere_frame.screen_scattering.image,
        atmosphere_frame.screen_transmittance.image,
        atmosphere_frame.screen_endpoint.image};
    std::array<VkImageMemoryBarrier,3> to_compute{};
    for(std::size_t index=0;index<to_compute.size();++index){
      auto& barrier=to_compute[index];
      barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcAccessMask=atmosphere_frame.screen_images_initialized?
          VK_ACCESS_SHADER_READ_BIT:0U;
      barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=reconstructed_images[index];
      barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount=1U;
      barrier.subresourceRange.layerCount=1U;
    }
    vkCmdPipelineBarrier(command_buffer,
        atmosphere_frame.screen_images_initialized?
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
        static_cast<std::uint32_t>(to_compute.size()),to_compute.data());
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        reference_hillaire?reference_hillaire_atmosphere_pipeline_:
                            faithful_atmosphere_pipeline_);
    vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        atmosphere_pipeline_layout_,0,1,&atmosphere_frame.descriptor_set,0,
        nullptr);
    std::uint64_t result_generation=hash_vectors(
        {atmosphere_input.camera_relative_world,
         atmosphere_input.camera_right,atmosphere_input.camera_down,
         atmosphere_input.camera_forward,atmosphere_input.sun_direction,
         atmosphere_input.planet_centre_relative_world},
        {atmosphere_input.vertical_tangent,atmosphere_input.aspect_ratio,
         static_cast<double>(surface_upload_planner_.published_generation()),
         static_cast<double>(shadow.atmosphere_shadow_depth_generation),
         static_cast<double>(atmosphere_optical_hash(parameters)),
         static_cast<double>(atmosphere_scattering_hash(parameters))});
    const std::uint32_t output_index=atmosphere_frame.history_write_index;
    const std::uint32_t previous_index=output_index^1U;
    AtmosphereScreenHistoryIdentity current_identity{
        .revisions=atmosphere.lookup_revisions.value_or(
            AtmosphereLookupRevisions{}),
        .terrain_generation=surface_upload_planner_.published_generation(),
        .result_generation=result_generation,
        .width=extent.width,.height=extent.height,
        .linear_resolution_divisor=screen_resolution_divisor_,
        .sample_count=temporal?2U:32U,
        .transport=atmosphere_input.transport,
        .rendering_method=atmosphere_input.rendering_method,
        .valid=atmosphere.lookup_revisions.has_value()};
    const auto compatibility=atmosphere_screen_history_compatibility(
        atmosphere_frame.history_identities[previous_index],current_identity);
    if(temporal){
      if(compatibility.compatible())
        ++atmosphere_dispatch_counts_.temporal_history_accepts;
      else ++atmosphere_dispatch_counts_.temporal_history_invalidations;
    }
    const std::uint32_t screen_width=
        (extent.width+screen_resolution_divisor_-1U)/
        screen_resolution_divisor_;
    const std::uint32_t screen_height=
        (extent.height+screen_resolution_divisor_-1U)/
        screen_resolution_divisor_;
    const std::uint32_t screen_groups_x=(screen_width+7U)/8U;
    const std::uint32_t screen_groups_y=(screen_height+7U)/8U;
    const std::array<std::uint32_t,4> push{
        12U,static_cast<std::uint32_t>(result_generation),
        screen_resolution_divisor_,0U};
    vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());
    vkCmdDispatch(command_buffer,screen_groups_x,screen_groups_y,1U);
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        timing_query_pool_,timing_base+4U);
    VkImageMemoryBarrier endpoint_ready=to_compute[2];
    endpoint_ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    endpoint_ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    endpoint_ready.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,
        &endpoint_ready);
    const std::uint32_t integration_samples=
        atmosphere_visibility_refresh_intervals(temporal,compatibility);
    const std::uint32_t visibility_control=temporal?
        (((atmosphere_frame.history_sequence/2U)+1U)&15U)|
        (previous_index<<5U)|(output_index<<6U)|
        (compatibility.compatible()?128U:0U)|256U|
        (compatibility.camera_changed?512U:0U):0U;
    const std::array<std::uint32_t,4> integration_push{
        13U,screen_resolution_divisor_,
        integration_samples,visibility_control};
    vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(integration_push),
        integration_push.data());
    vkCmdDispatch(command_buffer,screen_groups_x,screen_groups_y,1U);
    ++atmosphere_dispatch_counts_.screen_reconstruction;
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        timing_query_pool_,timing_base+5U);
    for(std::size_t index=0;index<to_compute.size();++index){
      auto& barrier=to_compute[index];
      barrier.srcAccessMask=index==2U?VK_ACCESS_SHADER_READ_BIT:
          VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        temporal?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,nullptr,0,nullptr,
        static_cast<std::uint32_t>(to_compute.size()),to_compute.data());
    if(temporal){
      if(reference_hillaire)
        vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
                          faithful_atmosphere_pipeline_);
      const std::array<VkImage,8> history_images{
          atmosphere_frame.history_scattering[0].image,
          atmosphere_frame.history_transmittance[0].image,
          atmosphere_frame.history_endpoint[0].image,
          atmosphere_frame.history_visibility[0].image,
          atmosphere_frame.history_scattering[1].image,
          atmosphere_frame.history_transmittance[1].image,
          atmosphere_frame.history_endpoint[1].image,
          atmosphere_frame.history_visibility[1].image};
      const std::uint32_t sample_count=
          compatibility.compatible()&&!compatibility.camera_changed?
          std::min(atmosphere_frame.history_sample_counts[previous_index]+1U,
                   8U):1U;
      const AtmosphereFrameResources::TemporalCameraSnapshot current_camera{
          .position_from_planet_centre_metres=camera_from_centre,
          .right=atmosphere_input.camera_right,
          .down=atmosphere_input.camera_down,
          .forward=atmosphere_input.camera_forward,
          .tangent_x=atmosphere_input.vertical_tangent*
              atmosphere_input.aspect_ratio,
          .tangent_y=atmosphere_input.vertical_tangent};
      const auto& previous_camera=compatibility.compatible()?
          atmosphere_frame.history_cameras[previous_index]:current_camera;
      std::array<float,20> temporal_uniform{};
      const auto vector=[&](std::size_t offset,const tetra::Vec3& value,
                            float fourth){
        temporal_uniform[offset]=static_cast<float>(value.x);
        temporal_uniform[offset+1U]=static_cast<float>(value.y);
        temporal_uniform[offset+2U]=static_cast<float>(value.z);
        temporal_uniform[offset+3U]=fourth;
      };
      vector(0U,previous_camera.position_from_planet_centre_metres,
             static_cast<float>(default_camera_near_plane*metres));
      vector(4U,previous_camera.right,
             static_cast<float>(previous_camera.tangent_x));
      vector(8U,previous_camera.down,
             static_cast<float>(previous_camera.tangent_y));
      vector(12U,previous_camera.forward,0.0F);
      temporal_uniform[16]=compatibility.compatible()?1.0F:0.0F;
      temporal_uniform[17]=std::bit_cast<float>(static_cast<std::uint32_t>(
          atmosphere_frame.history_identities[previous_index].result_generation));
      temporal_uniform[18]=static_cast<float>(sample_count-1U)/
          static_cast<float>(sample_count);
      void* temporal_mapped{};
      if(vkMapMemory(device_,atmosphere_frame.uniform_memory,0,
                     sizeof(float)*96U,0,&temporal_mapped)!=VK_SUCCESS)
        throw std::runtime_error("unable to map atmosphere temporal uniform");
      std::memcpy(static_cast<std::byte*>(temporal_mapped)+sizeof(float)*64U,
                  temporal_uniform.data(),sizeof(temporal_uniform));
      vkUnmapMemory(device_,atmosphere_frame.uniform_memory);

      const std::array<std::uint32_t,4> temporal_push{
          14U,previous_index,output_index,0U};
      vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
          VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(temporal_push),
          temporal_push.data());
      vkCmdDispatch(command_buffer,screen_groups_x,screen_groups_y,1U);
      std::array<VkImageMemoryBarrier,3> history_ready{};
      for(std::size_t channel=0;channel<3U;++channel){
        auto& barrier=history_ready[channel];
        barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=history_images[output_index*4U+channel];
        barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount=1U;
        barrier.subresourceRange.layerCount=1U;
      }
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
          static_cast<std::uint32_t>(history_ready.size()),
          history_ready.data());
      for(auto& barrier:to_compute){
        barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      }
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,
          static_cast<std::uint32_t>(to_compute.size()),to_compute.data());
      const std::array<std::uint32_t,4> publish_push{
          15U,output_index,0U,0U};
      vkCmdPushConstants(command_buffer,atmosphere_pipeline_layout_,
          VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(publish_push),
          publish_push.data());
      vkCmdDispatch(command_buffer,screen_groups_x,screen_groups_y,1U);
      for(auto& barrier:to_compute){
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
      }
      vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,
          static_cast<std::uint32_t>(to_compute.size()),to_compute.data());
      atmosphere_frame.history_identities[output_index]=current_identity;
      atmosphere_frame.history_cameras[output_index]=current_camera;
      atmosphere_frame.history_sample_counts[output_index]=sample_count;
      ++atmosphere_frame.history_sequence;
      atmosphere_frame.history_write_index=previous_index;
    }
    atmosphere_frame.screen_images_initialized=true;
  }else{
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timing_query_pool_,timing_base+4U);
    vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timing_query_pool_,timing_base+5U);
  }
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+6U);

  VkRenderingAttachmentInfo output_attachment{
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  output_attachment.imageView=colour_view;
  output_attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  output_attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  output_attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo output_rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  output_rendering.renderArea.extent=extent;
  output_rendering.layerCount=1;
  output_rendering.colorAttachmentCount=1;
  output_rendering.pColorAttachments=&output_attachment;
  begin_rendering(command_buffer,
      reinterpret_cast<const VkRenderingInfoKHR*>(&output_rendering));
  vkCmdSetViewport(command_buffer,0,1,&viewport);
  vkCmdSetScissor(command_buffer,0,1,&scissor);
  const VkPipeline selected_composite_pipeline=
      atmosphere_input.transport==AtmosphereTransport::qualified_baseline?
          composite_pipeline_:faithful_composite_pipeline_;
  vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
                    selected_composite_pipeline);
  vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
      composite_pipeline_layout_,0,1,&composite_descriptor_sets_.at(image_index),
      0,nullptr);
  vkCmdDraw(command_buffer,3U,1U,0U,0U);
  end_rendering(command_buffer);
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+7U);
  if(atmosphere_input.capture_requested){
    ensure_capture_resources(capture_frame,extent);
    VkImageMemoryBarrier to_capture{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_capture.srcAccessMask=capture_frame.initialized?
        VK_ACCESS_TRANSFER_READ_BIT:0U;
    to_capture.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_capture.oldLayout=capture_frame.initialized?
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    to_capture.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_capture.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    to_capture.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    to_capture.image=capture_frame.image;
    to_capture.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    to_capture.subresourceRange.levelCount=1;
    to_capture.subresourceRange.layerCount=1;
    vkCmdPipelineBarrier(command_buffer,
        capture_frame.initialized?VK_PIPELINE_STAGE_TRANSFER_BIT:
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,
        1,&to_capture);

    output_attachment.imageView=capture_frame.view;
    begin_rendering(command_buffer,
        reinterpret_cast<const VkRenderingInfoKHR*>(&output_rendering));
    vkCmdSetViewport(command_buffer,0,1,&viewport);
    vkCmdSetScissor(command_buffer,0,1,&scissor);
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
                      selected_composite_pipeline);
    vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
        composite_pipeline_layout_,0,1,
        &composite_descriptor_sets_.at(image_index),0,nullptr);
    vkCmdDraw(command_buffer,3U,1U,0U,0U);
    end_rendering(command_buffer);

    VkImageMemoryBarrier to_readback=to_capture;
    to_readback.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_readback.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    to_readback.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_readback.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&to_readback);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount=1;
    copy.imageExtent={extent.width,extent.height,1U};
    vkCmdCopyImageToBuffer(command_buffer,capture_frame.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,capture_frame.buffer,1,&copy);

    VkImageMemoryBarrier depth_to_readback{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    depth_to_readback.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;
    depth_to_readback.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    depth_to_readback.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth_to_readback.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    depth_to_readback.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    depth_to_readback.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    depth_to_readback.image=depth_images_.at(image_index).image;
    depth_to_readback.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_to_readback.subresourceRange.levelCount=1;
    depth_to_readback.subresourceRange.layerCount=1;
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,
        &depth_to_readback);
    copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(command_buffer,depth_images_.at(image_index).image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,capture_frame.depth_buffer,1,&copy);
    std::swap(depth_to_readback.srcAccessMask,
              depth_to_readback.dstAccessMask);
    std::swap(depth_to_readback.oldLayout,depth_to_readback.newLayout);
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,
        &depth_to_readback);

    std::array<VkBufferMemoryBarrier,2> host_reads{};
    for(auto& host_read:host_reads){
      host_read.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      host_read.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
      host_read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      host_read.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      host_read.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      host_read.size=VK_WHOLE_SIZE;
    }
    host_reads[0].buffer=capture_frame.buffer;
    host_reads[1].buffer=capture_frame.depth_buffer;
    vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,
        static_cast<std::uint32_t>(host_reads.size()),host_reads.data(),
        0,nullptr);
    capture_frame.initialized=true;
    capture_frame.pending=true;
  }
}

void SceneRenderer::shutdown() {
  if(timing_query_pool_!=VK_NULL_HANDLE)
    vkDestroyQueryPool(device_,timing_query_pool_,nullptr);
  for (auto& depth : depth_images_) { vkDestroyImageView(device_, depth.view, nullptr); vkDestroyImage(device_, depth.image, nullptr); vkFreeMemory(device_, depth.memory, nullptr); }
  for(auto& colour:scene_colour_images_){
    vkDestroyImageView(device_,colour.view,nullptr);
    vkDestroyImage(device_,colour.image,nullptr);
    vkFreeMemory(device_,colour.memory,nullptr);
  }
  vkDestroyImageView(device_,msaa_depth_image_.view,nullptr);
  vkDestroyImage(device_,msaa_depth_image_.image,nullptr);
  vkFreeMemory(device_,msaa_depth_image_.memory,nullptr);
  vkDestroyImageView(device_,msaa_colour_image_.view,nullptr);
  vkDestroyImage(device_,msaa_colour_image_.image,nullptr);
  vkFreeMemory(device_,msaa_colour_image_.memory,nullptr);
  for(auto& shadow:shadow_images_){
    for(auto view:shadow.layer_views)vkDestroyImageView(device_,view,nullptr);
    vkDestroyImageView(device_,shadow.view,nullptr);
    vkDestroyImage(device_,shadow.image,nullptr);
    vkFreeMemory(device_,shadow.memory,nullptr);
    vkDestroyBuffer(device_,shadow.uniform_buffer,nullptr);
    vkFreeMemory(device_,shadow.uniform_memory,nullptr);
    vkDestroyBuffer(device_,shadow.minmax_buffer,nullptr);
    vkFreeMemory(device_,shadow.minmax_memory,nullptr);
    vkDestroyBuffer(device_,shadow.epipolar_diagnostic_buffer,nullptr);
    vkFreeMemory(device_,shadow.epipolar_diagnostic_memory,nullptr);
  }
  for(auto* image:{&atmosphere_lookups_.transmittance,
                   &atmosphere_lookups_.multiple_scattering,
                   &atmosphere_lookups_.sky_view,
                   &atmosphere_lookups_.sky_irradiance,
                   &atmosphere_lookups_.aerial_scattering,
                   &atmosphere_lookups_.aerial_transmittance,
                   &atmosphere_lookups_.long_shadow}){
      vkDestroyImageView(device_,image->view,nullptr);
      vkDestroyImage(device_,image->image,nullptr);
      vkFreeMemory(device_,image->memory,nullptr);
  }
  for(auto& frame:atmosphere_frames_){
    vkDestroyBuffer(device_,frame.uniform_buffer,nullptr);
    vkFreeMemory(device_,frame.uniform_memory,nullptr);
    vkDestroyBuffer(device_,frame.probe_buffer,nullptr);
    vkFreeMemory(device_,frame.probe_memory,nullptr);
    for(auto* image:{&frame.screen_scattering,&frame.screen_transmittance,
                     &frame.screen_endpoint,&frame.froxel_scattering,
                     &frame.froxel_transmittance}){
      vkDestroyImageView(device_,image->view,nullptr);
      vkDestroyImage(device_,image->image,nullptr);
      vkFreeMemory(device_,image->memory,nullptr);
    }
    for(std::size_t history=0;history<2U;++history)
      for(auto* image:{&frame.history_scattering[history],
                       &frame.history_transmittance[history],
                       &frame.history_endpoint[history],
                       &frame.history_visibility[history]}){
        vkDestroyImageView(device_,image->view,nullptr);
        vkDestroyImage(device_,image->image,nullptr);
        vkFreeMemory(device_,image->memory,nullptr);
      }
  }
  for(auto& capture:capture_frames_){
    vkDestroyImageView(device_,capture.view,nullptr);
    vkDestroyImage(device_,capture.image,nullptr);
    vkFreeMemory(device_,capture.image_memory,nullptr);
    vkDestroyBuffer(device_,capture.buffer,nullptr);
    vkFreeMemory(device_,capture.buffer_memory,nullptr);
    vkDestroyBuffer(device_,capture.depth_buffer,nullptr);
    vkFreeMemory(device_,capture.depth_buffer_memory,nullptr);
  }
  for (const VertexBuffer& buffer : {triangles_, hierarchy_lines_, editor_lines_}) { vkDestroyBuffer(device_, buffer.buffer, nullptr); vkFreeMemory(device_, buffer.memory, nullptr); }
  if(descriptor_pool_!=VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
  vkDestroySampler(device_,shadow_sampler_,nullptr);
  vkDestroySampler(device_,scene_sampler_,nullptr);
  vkDestroySampler(device_,depth_sampler_,nullptr);
  vkDestroyDescriptorSetLayout(device_,descriptor_set_layout_,nullptr);
  vkDestroyDescriptorSetLayout(device_,composite_descriptor_set_layout_,nullptr);
  vkDestroyDescriptorSetLayout(device_,atmosphere_descriptor_set_layout_,nullptr);
  vkDestroyPipeline(device_,shadow_pipeline_,nullptr);
  vkDestroyPipeline(device_,sky_pipeline_,nullptr);
  vkDestroyPipeline(device_,composite_pipeline_,nullptr);
  vkDestroyPipeline(device_,faithful_composite_pipeline_,nullptr);
  vkDestroyPipeline(device_,atmosphere_pipeline_,nullptr);
  vkDestroyPipeline(device_,faithful_atmosphere_pipeline_,nullptr);
  vkDestroyPipeline(device_,reference_hillaire_atmosphere_pipeline_,nullptr);
  vkDestroyPipeline(device_, triangle_pipeline_, nullptr);
  vkDestroyPipeline(device_, triangle_wire_pipeline_, nullptr);
  vkDestroyPipeline(device_, line_pipeline_, nullptr);
  vkDestroyPipeline(device_, editor_line_pipeline_, nullptr);
  vkDestroyPipeline(device_,msaa_sky_pipeline_,nullptr);
  vkDestroyPipeline(device_,msaa_triangle_pipeline_,nullptr);
  vkDestroyPipeline(device_,msaa_triangle_wire_pipeline_,nullptr);
  vkDestroyPipeline(device_,msaa_line_pipeline_,nullptr);
  vkDestroyPipeline(device_,msaa_editor_line_pipeline_,nullptr);
  vkDestroyPipelineLayout(device_,shaded_pipeline_layout_,nullptr);
  vkDestroyPipelineLayout(device_,composite_pipeline_layout_,nullptr);
  vkDestroyPipelineLayout(device_,atmosphere_pipeline_layout_,nullptr);
  vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
}

}  // namespace tetra_viewer
