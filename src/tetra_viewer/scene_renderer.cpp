#include "scene_renderer.hpp"
#include "projection.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
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

  std::array<VkDescriptorSetLayoutBinding,11> composite_bindings{};
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

  std::array<VkDescriptorSetLayoutBinding,9> atmosphere_bindings{};
  for(std::uint32_t binding=0;binding<atmosphere_bindings.size();++binding){
    atmosphere_bindings[binding].binding=binding;
    atmosphere_bindings[binding].descriptorType=(binding<5U||binding==8U)?
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        (binding==6U?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
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
  const auto atmosphere_compute_shader=shader_module(
      device_,TETRA_VIEWER_SHADER_DIR "/atmosphere.comp.spv");
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
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering; pipeline.stageCount = 2; pipeline.pStages = pipeline_stages; pipeline.pVertexInputState = &vertex_input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &pipeline_blend; pipeline.pDynamicState = &dynamic; pipeline.layout = pipeline_layout;
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
                  shaded_pipeline_layout_,&triangle_pipeline_);
  // The surface wire pass rasterizes the exact same triangle vertices as the
  // opaque pass. Each edge is therefore a native one-pixel line between its
  // two endpoints, with identical depth interpolation and no depth pull.
  create_pipeline(wire_stages,VK_POLYGON_MODE_LINE,true,true,false,false,
                  pipeline_layout_,&triangle_wire_pipeline_);
  // Hierarchy edges are independent segments, expanded into screen-space
  // ribbons by edge.vert. They retain alpha coverage for antialiasing.
  create_pipeline(edge_stages,VK_POLYGON_MODE_FILL,true,true,true,false,
                  pipeline_layout_,&line_pipeline_);
  // Editor objects and manipulation handles remain visible when they overlap
  // the inspected mesh, as in conventional DCC applications. They use their
  // own buffer and pipeline so mesh-edge visibility remains depth correct.
  create_pipeline(edge_stages,VK_POLYGON_MODE_FILL,false,true,true,false,
                  pipeline_layout_,&editor_line_pipeline_);
  VkPipelineShaderStageCreateInfo sky_stages[2]{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
       VK_SHADER_STAGE_VERTEX_BIT,sky_vertex_shader,"main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
       VK_SHADER_STAGE_FRAGMENT_BIT,sky_fragment_shader,"main"}};
  create_pipeline(sky_stages,VK_POLYGON_MODE_FILL,false,false,false,false,
                  pipeline_layout_,&sky_pipeline_);

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
  shadow_raster.depthBiasConstantFactor=1.25F;
  shadow_raster.depthBiasSlopeFactor=1.75F;
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
  shadow_pipeline.pDynamicState=&dynamic;
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
  vkDestroyShaderModule(device_,atmosphere_compute_shader,nullptr);
}

void SceneRenderer::recreate(VkExtent2D extent, std::uint32_t image_count,
                             AtmosphereQuality quality) {
  quality_settings_=atmosphere_quality_settings(quality);
  atmosphere_dispatch_counts_={};
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
  for(auto& shadow:shadow_images_){
    for(auto view:shadow.layer_views)vkDestroyImageView(device_,view,nullptr);
    vkDestroyImageView(device_,shadow.view,nullptr);
    vkDestroyImage(device_,shadow.image,nullptr);
    vkFreeMemory(device_,shadow.memory,nullptr);
    vkDestroyBuffer(device_,shadow.uniform_buffer,nullptr);
    vkFreeMemory(device_,shadow.uniform_memory,nullptr);
  }
  for(auto* image:{&atmosphere_lookups_.transmittance,
                   &atmosphere_lookups_.multiple_scattering,
                   &atmosphere_lookups_.sky_view,
                   &atmosphere_lookups_.sky_irradiance,
                   &atmosphere_lookups_.aerial_scattering,
                   &atmosphere_lookups_.aerial_transmittance}){
      vkDestroyImageView(device_,image->view,nullptr);
      vkDestroyImage(device_,image->image,nullptr);
      vkFreeMemory(device_,image->memory,nullptr);
  }
  atmosphere_lookups_={};
  for(auto& frame:atmosphere_frames_){
    vkDestroyBuffer(device_,frame.uniform_buffer,nullptr);
    vkFreeMemory(device_,frame.uniform_memory,nullptr);
  }
  atmosphere_frames_.clear();
  shadow_images_.clear();
  descriptor_sets_.clear();
  composite_descriptor_sets_.clear();
  if(descriptor_pool_!=VK_NULL_HANDLE){
    vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
    descriptor_pool_=VK_NULL_HANDLE;
  }
  VkQueryPoolCreateInfo timing_pool{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  timing_pool.queryType=VK_QUERY_TYPE_TIMESTAMP;
  timing_pool.queryCount=image_count*5U;
  if(vkCreateQueryPool(device_,&timing_pool,nullptr,&timing_query_pool_)!=
     VK_SUCCESS)
    throw std::runtime_error("unable to create scene timing query pool");
  timing_queries_written_.assign(image_count,false);
  gpu_timings_={};
  const auto frames=static_cast<std::size_t>(image_count);
  scene_target_allocation_bytes_=frames*
      static_cast<std::size_t>(extent.width)*extent.height*(8U+4U);
  const std::size_t lookup_bytes=
      quality_settings_.transmittance_width*
          quality_settings_.transmittance_height*8U+
      quality_settings_.multiple_scattering_size*
          quality_settings_.multiple_scattering_size*8U+
      quality_settings_.sky_width*quality_settings_.sky_height*8U+
      quality_settings_.irradiance_width*
          quality_settings_.irradiance_height*8U+
      2U*quality_settings_.aerial_width*quality_settings_.aerial_height*
          quality_settings_.aerial_depth*8U;
  const std::size_t shadow_bytes=shadow_cascade_count*
      quality_settings_.shadow_resolution*
      quality_settings_.shadow_resolution*4U;
  atmosphere_allocation_bytes_=lookup_bytes+frames*shadow_bytes;
  depth_images_.assign(image_count, {});
  for (auto& depth : depth_images_) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = depth_format_; image.extent = {extent.width, extent.height, 1}; image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

  const auto make_atmosphere_image=[this](AtmosphereImage& destination,
                                           VkImageType image_type,
                                           VkImageViewType view_type,
                                           VkExtent3D image_extent){
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=image_type;
    image.format=VK_FORMAT_R16G16B16A16_SFLOAT;
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
    view.format=VK_FORMAT_R16G16B16A16_SFLOAT;
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
  for(auto& frame:atmosphere_frames_){
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=sizeof(float)*64U;
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
  }

  const std::uint32_t shadow_extent=quality_settings_.shadow_resolution;
  shadow_images_.resize(image_count);
  for(auto& shadow:shadow_images_){
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D;
    image.format=depth_format_;
    image.extent={shadow_extent,shadow_extent,1U};
    image.mipLevels=1;
    image.arrayLayers=static_cast<std::uint32_t>(shadow_cascade_count);
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
        shadow_cascade_count);
    if(vkCreateImageView(device_,&view,nullptr,&shadow.view)!=VK_SUCCESS)
      throw std::runtime_error("unable to create sun shadow view");
    for(std::uint32_t layer=0;layer<shadow_cascade_count;++layer){
      view.viewType=VK_IMAGE_VIEW_TYPE_2D;
      view.subresourceRange.baseArrayLayer=layer;
      view.subresourceRange.layerCount=1;
      if(vkCreateImageView(device_,&view,nullptr,
                           &shadow.layer_views[layer])!=VK_SUCCESS)
        throw std::runtime_error("unable to create sun shadow cascade view");
    }
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=sizeof(float)*(16U*shadow_cascade_count+4U);
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
  }

  const std::array<VkDescriptorPoolSize,3> pool_sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           image_count*14U},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,image_count*6U},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,image_count*6U}};
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
        sizeof(float)*(16U*shadow_cascade_count+4U)};
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
        frame.uniform_buffer,0,sizeof(float)*64U};
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

    const std::array<VkDescriptorImageInfo,9> composite_images{
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
                              VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo uniform_info{frame.uniform_buffer,0,
                                        sizeof(float)*64U};
    std::array<VkWriteDescriptorSet,11> composite_writes{};
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
            (binding==8U?7U:8U);
        descriptor.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor.pImageInfo=&composite_images[image_index];
      }
    }
    vkUpdateDescriptorSets(device_,
        static_cast<std::uint32_t>(composite_writes.size()),
        composite_writes.data(),0,nullptr);

    const std::array<VkImageView,6> storage_views{
        atmosphere.transmittance.view,atmosphere.multiple_scattering.view,
        atmosphere.sky_view.view,atmosphere.aerial_scattering.view,
        atmosphere.aerial_transmittance.view,
        atmosphere.sky_irradiance.view};
    std::array<VkDescriptorImageInfo,6> storage_images{};
    std::array<VkWriteDescriptorSet,9> atmosphere_writes{};
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

  constexpr std::uint32_t timing_count=5U;
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
      gpu_timings_={elapsed(0,1),elapsed(1,2),elapsed(2,3),elapsed(3,4),true};
    }
  }
  vkCmdResetQueryPool(command_buffer,timing_query_pool_,timing_base,timing_count);
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      timing_query_pool_,timing_base);
  timing_queries_written_[image_index]=true;

  auto& atmosphere_frame=atmosphere_frames_.at(image_index);
  auto& atmosphere=atmosphere_lookups_;
  const auto& parameters=atmosphere_input.parameters;
  const bool atmosphere_valid=!validate_atmosphere(parameters).has_value();
  const bool atmosphere_enabled=atmosphere_input.enabled&&atmosphere_valid&&
                                !black_clear;
  const double metres=parameters.metres_per_world_unit;
  const auto camera_from_centre=
      (atmosphere_input.camera_relative_world-
       atmosphere_input.planet_centre_relative_world)*metres;
  const double local_aerial_distance=atmosphere_local_aerial_distance(
      parameters,std::sqrt(camera_from_centre.x*camera_from_centre.x+
          camera_from_centre.y*camera_from_centre.y+
          camera_from_centre.z*camera_from_centre.z)-
          parameters.ground_radius_metres,
      atmosphere_input.maximum_aerial_distance_metres);
  std::array<float,64> atmosphere_uniform{};
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
  atmosphere_uniform[53]=static_cast<float>(atmosphere_input.transport==
      AtmosphereTransport::faithful_hillaire?1:0);
  void* mapped{};
  if(vkMapMemory(device_,atmosphere_frame.uniform_memory,0,
                 sizeof(atmosphere_uniform),0,&mapped)!=VK_SUCCESS)
    throw std::runtime_error("unable to map atmosphere uniform buffer");
  std::memcpy(mapped,atmosphere_uniform.data(),sizeof(atmosphere_uniform));
  vkUnmapMemory(device_,atmosphere_frame.uniform_memory);

  auto& shadow=shadow_images_.at(image_index);
  const auto cascades=make_stable_shadow_cascades(
      atmosphere_input.camera_relative_world,atmosphere_input.camera_forward,
      atmosphere_input.sun_direction,quality_settings_.shadow_resolution);
  std::array<float,16U*shadow_cascade_count+4U> shadow_uniform{};
  for(std::size_t cascade=0;cascade<shadow_cascade_count;++cascade){
    std::copy(cascades.cascades[cascade].matrix.begin(),
              cascades.cascades[cascade].matrix.end(),
              shadow_uniform.begin()+cascade*16U);
    shadow_uniform[16U*shadow_cascade_count+cascade]=static_cast<float>(
        cascades.cascades[cascade].split_distance);
  }
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
  to_shadow.newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  to_shadow.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_shadow.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  to_shadow.image=shadow.image;
  to_shadow.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
  to_shadow.subresourceRange.levelCount=1;
  to_shadow.subresourceRange.layerCount=static_cast<std::uint32_t>(
      shadow_cascade_count);
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
  for(std::size_t cascade=0;cascade<shadow_cascade_count;++cascade){
    VkRenderingAttachmentInfo shadow_attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    shadow_attachment.imageView=shadow.layer_views[cascade];
    shadow_attachment.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    shadow_attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadow_attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    shadow_attachment.clearValue=shadow_clear;
    VkRenderingInfo shadow_rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    shadow_rendering.renderArea.extent={quality_settings_.shadow_resolution,
                                       quality_settings_.shadow_resolution};
    shadow_rendering.layerCount=1;
    shadow_rendering.pDepthAttachment=&shadow_attachment;
    begin_rendering(command_buffer,
        reinterpret_cast<const VkRenderingInfoKHR*>(&shadow_rendering));
    vkCmdSetViewport(command_buffer,0,1,&shadow_viewport);
    vkCmdSetScissor(command_buffer,0,1,&shadow_scissor);
    if(triangles_.count!=0U){
      VkDeviceSize shadow_offset{};
      vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadow_pipeline_);
      std::array<float,28> shadow_push{};
      std::copy(cascades.cascades[cascade].matrix.begin(),
                cascades.cascades[cascade].matrix.end(),shadow_push.begin());
      vkCmdPushConstants(command_buffer,pipeline_layout_,
                         VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(float)*28,
                         shadow_push.data());
      vkCmdBindVertexBuffers(command_buffer,0,1,&triangles_.buffer,
                             &shadow_offset);
      const auto ranges=surface_upload_planner_.published_draws();
      if(ranges.empty())
        vkCmdDraw(command_buffer,static_cast<std::uint32_t>(triangles_.count),1,
                  0,0);
      else for(const auto range:ranges)
        vkCmdDraw(command_buffer,static_cast<std::uint32_t>(range.vertex_count),
                  1,static_cast<std::uint32_t>(range.first_vertex),0);
    }
    end_rendering(command_buffer);
  }

  VkImageMemoryBarrier to_sample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_sample.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_sample.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  to_sample.oldLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
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

  const std::array<VkImage,6> atmosphere_images{
      atmosphere.transmittance.image,atmosphere.multiple_scattering.image,
      atmosphere.sky_view.image,atmosphere.aerial_scattering.image,
      atmosphere.aerial_transmittance.image,atmosphere.sky_irradiance.image};
  if(!atmosphere.images_initialized){
    std::array<VkImageMemoryBarrier,6> initialize_barriers{};
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
  }
  if(atmosphere_enabled){
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_pipeline_);
    vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,
        atmosphere_pipeline_layout_,0,1,&atmosphere_frame.descriptor_set,0,
        nullptr);
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
    const AtmosphereLookupRevisions next_revisions{
        .optical={atmosphere_optical_hash(parameters)},
        .scattering={atmosphere_scattering_hash(parameters)},
        .sun={hash_vectors({atmosphere_input.sun_direction})},
        .camera_position={hash_vectors({camera_from_centre})},
        .sky_position=atmosphere_sky_position_revision(
            camera_from_centre,parameters),
        .camera_orientation={hash_vectors(
            {atmosphere_input.camera_right,atmosphere_input.camera_down,
             atmosphere_input.camera_forward},
            {atmosphere_input.vertical_tangent,atmosphere_input.aspect_ratio})},
        .shadow={geometry_revision_},
        .render_origin={hash_vectors(
            {atmosphere_input.planet_centre_relative_world})}};
    const auto previous_revisions=atmosphere.transport==
        atmosphere_input.transport?atmosphere.lookup_revisions:std::nullopt;
    const auto plan=atmosphere_dispatch_plan(previous_revisions,next_revisions,
        atmosphere_input.transport);
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
    if(plan.sky_view)
      dispatch(2U,(quality_settings_.sky_width+7U)/8U,
               (quality_settings_.sky_height+7U)/8U,1U);
    if(plan.sky_irradiance&&
       atmosphere_input.transport==AtmosphereTransport::faithful_hillaire){
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
    compute_barrier(atmosphere_images,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    atmosphere.lookup_revisions=next_revisions;
    atmosphere.transport=atmosphere_input.transport;
  }
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+2U);

  auto& scene_colour=scene_colour_images_.at(image_index);
  auto& scene_depth=depth_images_.at(image_index);
  std::array<VkImageMemoryBarrier,2> to_scene{};
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
  to_scene[1].newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  to_scene[1].image=scene_depth.image;
  to_scene[1].subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
  vkCmdPipelineBarrier(command_buffer,
      (scene_colour.initialized||scene_depth.initialized)?
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      0,0,nullptr,0,nullptr,static_cast<std::uint32_t>(to_scene.size()),
      to_scene.data());

  VkClearValue colour{};
  colour.color=black_clear?VkClearColorValue{{0.0F,0.0F,0.0F,1.0F}}:
      VkClearColorValue{{0.06F,0.08F,0.11F,1.0F}};
  VkClearValue depth{};
  depth.depthStencil={depth_clear(main_scene_depth_convention),0U};
  VkRenderingAttachmentInfo colour_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; colour_attachment.imageView = scene_colour.view; colour_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; colour_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colour_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; colour_attachment.clearValue = colour;
  VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth_attachment.imageView = scene_depth.view; depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; depth_attachment.clearValue = depth;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = extent; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &colour_attachment; rendering.pDepthAttachment = &depth_attachment;
  begin_rendering(command_buffer, reinterpret_cast<const VkRenderingInfoKHR*>(&rendering));
  VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F}; VkRect2D scissor{{0, 0}, extent}; VkDeviceSize offset{};
  vkCmdSetViewport(command_buffer, 0, 1, &viewport); vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  std::array<float,28> push_data{};
  std::copy_n(camera_data,28,push_data.begin());
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
    vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,sky_pipeline_);
    vkCmdPushConstants(command_buffer,pipeline_layout_,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,
        sizeof(float)*28,push_data.data());
    vkCmdDraw(command_buffer,6U,1U,0U,0U);
  }
  draw(triangle_pipeline_,shaded_pipeline_layout_,triangles_,
       surface_upload_planner_.published_draws());
  draw(triangle_wire_pipeline_,pipeline_layout_,triangles_,
       surface_upload_planner_.published_draws());
  push_data[24]=static_cast<float>(extent.width);
  push_data[25]=static_cast<float>(extent.height);
  // One-pixel half-extent provides the complete filter footprint for an
  // analytically antialiased one-pixel centre line.
  push_data[26]=1.0F;
  push_data[27]=0.0F;
  draw(line_pipeline_,pipeline_layout_,hierarchy_lines_);
  draw(editor_line_pipeline_,pipeline_layout_,editor_lines_);
  end_rendering(command_buffer);
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
  to_composite[1].oldLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  to_composite[1].image=scene_depth.image;
  to_composite[1].subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
  vkCmdPipelineBarrier(command_buffer,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,
      static_cast<std::uint32_t>(to_composite.size()),to_composite.data());
  scene_colour.initialized=true;
  scene_depth.initialized=true;

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
  vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
                    composite_pipeline_);
  vkCmdBindDescriptorSets(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
      composite_pipeline_layout_,0,1,&composite_descriptor_sets_.at(image_index),
      0,nullptr);
  vkCmdDraw(command_buffer,3U,1U,0U,0U);
  end_rendering(command_buffer);
  vkCmdWriteTimestamp(command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      timing_query_pool_,timing_base+4U);
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
  for(auto& shadow:shadow_images_){
    for(auto view:shadow.layer_views)vkDestroyImageView(device_,view,nullptr);
    vkDestroyImageView(device_,shadow.view,nullptr);
    vkDestroyImage(device_,shadow.image,nullptr);
    vkFreeMemory(device_,shadow.memory,nullptr);
    vkDestroyBuffer(device_,shadow.uniform_buffer,nullptr);
    vkFreeMemory(device_,shadow.uniform_memory,nullptr);
  }
  for(auto* image:{&atmosphere_lookups_.transmittance,
                   &atmosphere_lookups_.multiple_scattering,
                   &atmosphere_lookups_.sky_view,
                   &atmosphere_lookups_.sky_irradiance,
                   &atmosphere_lookups_.aerial_scattering,
                   &atmosphere_lookups_.aerial_transmittance}){
      vkDestroyImageView(device_,image->view,nullptr);
      vkDestroyImage(device_,image->image,nullptr);
      vkFreeMemory(device_,image->memory,nullptr);
  }
  for(auto& frame:atmosphere_frames_){
    vkDestroyBuffer(device_,frame.uniform_buffer,nullptr);
    vkFreeMemory(device_,frame.uniform_memory,nullptr);
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
  vkDestroyPipeline(device_,atmosphere_pipeline_,nullptr);
  vkDestroyPipeline(device_, triangle_pipeline_, nullptr);
  vkDestroyPipeline(device_, triangle_wire_pipeline_, nullptr);
  vkDestroyPipeline(device_, line_pipeline_, nullptr);
  vkDestroyPipeline(device_, editor_line_pipeline_, nullptr);
  vkDestroyPipelineLayout(device_,shaded_pipeline_layout_,nullptr);
  vkDestroyPipelineLayout(device_,composite_pipeline_layout_,nullptr);
  vkDestroyPipelineLayout(device_,atmosphere_pipeline_layout_,nullptr);
  vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
}

}  // namespace tetra_viewer
