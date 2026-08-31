// Dear ImGui: standalone example application for Glfw + Vulkan

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// Important note to the reader who wish to integrate imgui_impl_vulkan.cpp/.h in their own engine/app.
// - Common ImGui_ImplVulkan_XXX functions and structures are used to interface with imgui_impl_vulkan.cpp/.h.
//   You will use those if you want to use this rendering backend in your engine/app.
// - Helper ImGui_ImplVulkanH_XXX functions and structures are only used by this example (main.cpp) and by
//   the backend itself (imgui_impl_vulkan.cpp), but should PROBABLY NOT be used by your own engine/app code.
// Read comments in imgui_impl_vulkan.h.

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "tetra_core/tet_mesh.hpp"
#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/adjacency.hpp"
#include "tetra_core/layer_storage.hpp"
#include "scene_renderer.hpp"
#include "tetra_viewer/application.hpp"
#include "tetra_viewer/atmosphere.hpp"
#include "tetra_viewer/camera_manipulator.hpp"
#include "tetra_viewer/first_person_controller.hpp"
#include "tetra_viewer/image_oracle.hpp"
#include "tetra_viewer/mesh_update_worker.hpp"
#include "tetra_viewer/projection.hpp"
#include "tetra_viewer/scene_preparation_worker.hpp"
#include "tetra_viewer/terrain_runtime.hpp"
#include "tetra_viewer/world_script.hpp"
#include "tetra_viewer/viewer_script.hpp"
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// Volk headers
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

//#define APP_USE_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
#endif

// Data
static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static int                      g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;
static tetra_viewer::SceneRenderer g_SceneRenderer;
static std::array<float, 28> g_CameraData{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
static bool g_BlackSceneClear = false;
static tetra_viewer::AtmosphereFrameInput g_AtmosphereFrame;

static bool CheckboxWithHotkey(const char* label,const char* hotkey,
                               ImGuiKey key,bool* value)
{
    const std::string visible_label=std::string(label)+" ("+hotkey+")";
    bool changed=ImGui::Checkbox(visible_label.c_str(),value);
    const auto& input=ImGui::GetIO();
    if(!input.WantTextInput&&!ImGui::IsAnyItemActive()&&
       ImGui::IsKeyPressed(key,false)){
        *value=!*value;
        changed=true;
    }
    return changed;
}

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
static void check_vk_result(VkResult err)
{
    if (err == 0)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData)
{
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix; // Unused arguments
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif // APP_USE_VULKAN_DEBUG_REPORT

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
{
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static VkPhysicalDevice SetupVulkan_SelectPhysicalDevice()
{
    uint32_t gpu_count;
    VkResult err = vkEnumeratePhysicalDevices(g_Instance, &gpu_count, nullptr);
    check_vk_result(err);
    IM_ASSERT(gpu_count > 0);

    ImVector<VkPhysicalDevice> gpus;
    gpus.resize(gpu_count);
    err = vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus.Data);
    check_vk_result(err);

    // If a number >1 of GPUs got reported, find discrete GPU if present, or use first one available. This covers
    // most common cases (multi-gpu/integrated+dedicated graphics). Handling more complicated setups (multiple
    // dedicated GPUs) is out of scope of this sample.
    for (VkPhysicalDevice& device : gpus)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            return device;
    }

    // Use first GPU (Integrated) is a Discrete one is not available.
    if (gpu_count > 0)
        return gpus[0];
    return VK_NULL_HANDLE;
}

static void SetupVulkan(ImVector<const char*> instance_extensions)
{
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Enumerate available extensions
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        // Enable required extensions
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // Enabling validation layers
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        // Create Vulkan Instance
        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

        // Setup the debug report callback
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif
    }

    // Select Physical Device (GPU)
    g_PhysicalDevice = SetupVulkan_SelectPhysicalDevice();

    // Select graphics queue family
    {
        uint32_t count;
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
        VkQueueFamilyProperties* queues = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * count);
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, queues);
        for (uint32_t i = 0; i < count; i++)
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                g_QueueFamily = i;
                break;
            }
        free(queues);
        IM_ASSERT(g_QueueFamily != (uint32_t)-1);
    }

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
        if (IsExtensionAvailable(properties, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
        {
            if (IsExtensionAvailable(properties, "VK_KHR_multiview"))
                device_extensions.push_back("VK_KHR_multiview");
            if (IsExtensionAvailable(properties, "VK_KHR_maintenance2"))
                device_extensions.push_back("VK_KHR_maintenance2");
            if (IsExtensionAvailable(properties, "VK_KHR_depth_stencil_resolve"))
                device_extensions.push_back("VK_KHR_depth_stencil_resolve");
            if (IsExtensionAvailable(properties, "VK_KHR_create_renderpass2"))
                device_extensions.push_back("VK_KHR_create_renderpass2");
            device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        }
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#else
        if (IsExtensionAvailable(properties, "VK_KHR_portability_subset"))
            device_extensions.push_back("VK_KHR_portability_subset");
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        VkPhysicalDeviceFeatures supported_features{};
        vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &supported_features);
        if (supported_features.fillModeNonSolid != VK_TRUE)
        {
            fprintf(stderr, "The selected Vulkan device cannot render native triangle wireframes (fillModeNonSolid is unavailable).\n");
            exit(-1);
        }
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.fillModeNonSolid = VK_TRUE;
        create_info.pEnabledFeatures = &enabled_features;
        VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        dynamic_rendering.dynamicRendering = VK_TRUE;
        create_info.pNext = &dynamic_rendering;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // Create Descriptor Pool
    // The example only requires a single combined image sampler descriptor for the font image and only uses one descriptor set (for that)
    // If you wish to load e.g. additional textures you may need to alter pools sizes.
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

// All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
// Your real engine/app may not use them.
static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;

    // Check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    // Select Surface Format
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // Select Present Mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
    //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

    // Create SwapChain, RenderPass, Framebuffer, etc.
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupVulkan()
{
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // Remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow()
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
{
    VkResult err;

    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    VkImageMemoryBarrier backbuffer_to_colour{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    backbuffer_to_colour.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // UNDEFINED is intentional here: the scene overwrites the complete image,
    // so preserving the previously presented contents would add no value.
    backbuffer_to_colour.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    backbuffer_to_colour.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    backbuffer_to_colour.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backbuffer_to_colour.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backbuffer_to_colour.image = fd->Backbuffer;
    backbuffer_to_colour.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    backbuffer_to_colour.subresourceRange.levelCount = 1;
    backbuffer_to_colour.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &backbuffer_to_colour);
    const VkExtent2D extent{static_cast<std::uint32_t>(wd->Width), static_cast<std::uint32_t>(wd->Height)};
    g_SceneRenderer.record(fd->CommandBuffer,fd->BackbufferView,wd->FrameIndex,
                           extent,g_CameraData.data(),g_AtmosphereFrame,
                           g_BlackSceneClear);
    VkRenderingAttachmentInfo overlay{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    overlay.imageView = fd->BackbufferView;
    overlay.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlay.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    overlay.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo overlay_info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    overlay_info.renderArea.extent = extent;
    overlay_info.layerCount = 1;
    overlay_info.colorAttachmentCount = 1;
    overlay_info.pColorAttachments = &overlay;
    const auto begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(g_Device, "vkCmdBeginRenderingKHR"));
    const auto end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(g_Device, "vkCmdEndRenderingKHR"));
    IM_ASSERT(begin_rendering != nullptr && end_rendering != nullptr);
    begin_rendering(fd->CommandBuffer, reinterpret_cast<const VkRenderingInfoKHR*>(&overlay_info));
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    end_rendering(fd->CommandBuffer);
    VkImageMemoryBarrier backbuffer_to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    backbuffer_to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    backbuffer_to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    backbuffer_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    backbuffer_to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backbuffer_to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backbuffer_to_present.image = fd->Backbuffer;
    backbuffer_to_present.subresourceRange = backbuffer_to_colour.subresourceRange;
    vkCmdPipelineBarrier(fd->CommandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &backbuffer_to_present);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount; // Now we can use the next set of semaphores
}

// Main code
int tetra_viewer::run_application(int argc, char** argv,ApplicationMode mode)
{
    const bool world_mode=mode==ApplicationMode::world;
    g_AtmosphereFrame={};
    g_AtmosphereFrame.enabled=world_mode;
    if(world_mode)
        g_AtmosphereFrame.parameters=tetra_viewer::atmosphere_preset(
            tetra_viewer::default_world_atmosphere_preset);
    g_AtmosphereFrame.parameters.metres_per_world_unit=10.0;
    if(world_mode)g_AtmosphereFrame.maximum_aerial_distance_metres=
        tetra_viewer::default_world_aerial_distance_metres;
    // The headless path intentionally returns before any platform Vulkan
    // loading, GLFW initialization, or window creation.
    if (argc >= 2 && strcmp(argv[1], "--script-help") == 0) {
        if (argc != 2) {
            fprintf(stderr, "--script-help does not accept arguments\n");
            return 2;
        }
        if(world_mode)tetra_viewer::print_world_script_help(std::cout);
        else tetra_viewer::print_script_help(std::cout);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--script") == 0) {
        if (argc != 3) {
            fprintf(stderr, "usage: %s --script \"command[,command...]\"\n",
                    world_mode?"tetra_world":"tetra_viewer");
            return 2;
        }
        return world_mode?
            tetra_viewer::run_world_script(argv[2],std::cout,std::cerr):
            tetra_viewer::run_script(argv[2], std::cout, std::cerr);
    }
    if(world_mode&&argc>=2&&strcmp(argv[1],"--runtime-benchmark")==0){
        if(argc!=2){
            fprintf(stderr,"--runtime-benchmark does not accept arguments\n");
            return 2;
        }
        return tetra_viewer::run_world_runtime_benchmark(std::cout,std::cerr);
    }
    if(world_mode&&argc>=2&&strcmp(argv[1],"--atmosphere-check")==0){
        if(argc<2||argc>6){
            fprintf(stderr,"usage: tetra_world --atmosphere-check [preset] "
                    "[camera-altitude-metres] [view-zenith-degrees] "
                    "[sun-zenith-degrees]\n");
            return 2;
        }
        const auto preset=argc>=3?
            tetra_viewer::parse_atmosphere_preset(argv[2]):
            std::optional{tetra_viewer::AtmospherePreset::earth};
        if(!preset){
            fprintf(stderr,"unknown atmosphere preset\n");
            return 2;
        }
        std::array<double,3> values{0.0,0.0,45.0};
        for(int index=3;index<argc;++index){
            char* end=nullptr;
            values[static_cast<std::size_t>(index-3)]=std::strtod(argv[index],&end);
            if(end==argv[index]||*end!='\0'||
               !std::isfinite(values[static_cast<std::size_t>(index-3)])){
                fprintf(stderr,"atmosphere check values must be finite numbers\n");
                return 2;
            }
        }
        return tetra_viewer::run_atmosphere_check(
            *preset,values[0],values[1],values[2],std::cout,std::cerr);
    }
    if(world_mode&&argc>=2&&strcmp(argv[1],"--capture")==0){
        if(argc!=3){
            fprintf(stderr,"usage: tetra_world --capture <path.ppm>\n");
            return 2;
        }
        return tetra_viewer::capture_world_runtime(argv[2],std::cout,std::cerr);
    }
    if(world_mode&&argc>=2&&strcmp(argv[1],"--capture-view")==0){
        if(argc!=9){
            fprintf(stderr,"usage: tetra_world --capture-view <path.ppm> "
                    "<camera-x> <camera-y> <camera-z> <target-x> <target-y> <target-z>\n");
            return 2;
        }
        std::array<double,6> values{};
        for(std::size_t index=0;index<values.size();++index){
            char* end=nullptr;
            values[index]=std::strtod(argv[index+3U],&end);
            if(end==argv[index+3U]||*end!='\0'||!std::isfinite(values[index])){
                fprintf(stderr,"capture view coordinates must be finite numbers\n");
                return 2;
            }
        }
        return tetra_viewer::capture_world_runtime_view(
            argv[2],{values[0],values[1],values[2]},
            {values[3],values[4],values[5]},std::cout,std::cerr);
    }
    std::size_t geometry_worker_count=tetra::default_geometry_worker_count();
    constexpr std::string_view geometry_workers_prefix="--geometry-workers=";
    constexpr std::string_view window_size_prefix="--window-size=";
    std::array<std::uint32_t,2> initial_window_size{1280U,800U};
    for(int argument=1;argument<argc;++argument){
        const std::string_view value=argv[argument];
        if(value.starts_with(window_size_prefix)){
            const auto extent=tetra_viewer::parse_pixel_extent(
                value.substr(window_size_prefix.size()));
            if(!extent){
                fprintf(stderr,"window size must be WIDTHxHEIGHT in [64,8192]\n");
                return 2;
            }
            initial_window_size=*extent;
            continue;
        }
        if(!value.starts_with(geometry_workers_prefix))continue;
        const auto count=value.substr(geometry_workers_prefix.size());
        const std::string count_text(count);
        char* parsed_end=nullptr;
        const auto parsed=std::strtoul(count_text.c_str(),&parsed_end,10);
        if(count.empty()||parsed_end==nullptr||*parsed_end!='\0'||parsed==0U||parsed>64U){
            fprintf(stderr,"geometry worker count must be in [1,64]\n");
            return 2;
        }
        geometry_worker_count=static_cast<std::size_t>(parsed);
    }
#if defined(__APPLE__)
    // A terminal launch does not inherit the development shell's Vulkan ICD
    // setting. Homebrew's stock manifest uses a relative driver path, which
    // only resolves when DYLD_LIBRARY_PATH is set. Give the loader a small
    // absolute-path manifest instead.
    constexpr const char* molten_vk_library = "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib";
    constexpr const char* vulkan_loader_library = "/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib";
    if (std::filesystem::exists(vulkan_loader_library)) dlopen(vulkan_loader_library, RTLD_NOW | RTLD_GLOBAL);
    if (std::getenv("VK_ICD_FILENAMES") == nullptr) {
        if (std::filesystem::exists(molten_vk_library)) {
            const auto manifest = std::filesystem::temp_directory_path() / "tetra_viewer_MoltenVK_icd.json";
            std::ofstream file(manifest, std::ios::trunc);
            file << "{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n"
                 << "    \"library_path\": \"" << molten_vk_library << "\",\n"
                 << "    \"api_version\": \"1.4.0\",\n"
                 << "    \"is_portability_driver\": true\n  }\n}\n";
            file.close();
            setenv("VK_ICD_FILENAMES", manifest.c_str(), 0);
        }
    }
#endif
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // Create window with Vulkan context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(
        static_cast<int>(initial_window_size[0]),
        static_cast<int>(initial_window_size[1]),
        world_mode?"Tetra World":"Tetrahedral refinement",nullptr,nullptr);
    if (argc > 1 && (strcmp(argv[1], "--whole-cell-check") == 0 ||
                    strcmp(argv[1], "--whole-cell-cutaway-check") == 0))
        glfwSetWindowPos(window, 20, 40);
    if (!glfwVulkanSupported())
    {
        printf("GLFW: Vulkan Not Supported\n");
        return 1;
    }

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    for (uint32_t i = 0; i < extensions_count; i++)
        extensions.push_back(glfw_extensions[i]);
    SetupVulkan(extensions);

    // Create Window Surface
    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    check_vk_result(err);

    // Create Framebuffers
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    wd->UseDynamicRendering = true;
    SetupVulkanWindow(wd, surface, w, h);
    g_SceneRenderer.initialize(g_PhysicalDevice, g_Device, wd->SurfaceFormat.format, VK_FORMAT_D32_SFLOAT);
    g_SceneRenderer.recreate(
        {static_cast<std::uint32_t>(wd->Width),
         static_cast<std::uint32_t>(wd->Height)},wd->ImageCount,
        g_AtmosphereFrame.quality,g_AtmosphereFrame.screen_resolution_divisor);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.UseDynamicRendering = true;
    init_info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &wd->SurfaceFormat.format;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);

    const bool wireframe_check = argc > 1 && strcmp(argv[1], "--wireframe-check") == 0;
    const bool tetweave_cutaway_check = argc > 1 && strcmp(argv[1], "--tetweave-cutaway-check") == 0;
    const bool selected_atlas_check = argc > 1 && strcmp(argv[1], "--selected-atlas-check") == 0;
    const bool whole_cell_check = argc > 1 && strcmp(argv[1], "--whole-cell-check") == 0;
    const bool whole_cell_cutaway_check = argc > 1 && strcmp(argv[1], "--whole-cell-cutaway-check") == 0;
    const bool manipulator_close_check = argc > 1 && strcmp(argv[1], "--manipulator-close-check") == 0;
    const bool manipulator_distant_check = argc > 1 && strcmp(argv[1], "--manipulator-distant-check") == 0;
    const bool manipulator_edge_on_check = argc > 1 && strcmp(argv[1], "--manipulator-edge-on-check") == 0;
    const bool manipulator_panel_check = argc > 1 && strcmp(argv[1], "--manipulator-panel-check") == 0;
    const bool manipulator_move_check = argc > 1 &&
        (strcmp(argv[1], "--manipulator-move-check") == 0||manipulator_close_check||
         manipulator_distant_check||manipulator_edge_on_check||manipulator_panel_check);
    const bool manipulator_rotate_check = argc > 1 && strcmp(argv[1], "--manipulator-rotate-check") == 0;
    const bool retained_upload_check = argc > 1 &&
        strcmp(argv[1], "--retained-upload-check") == 0;
    const bool manipulator_visual_check=manipulator_move_check||manipulator_rotate_check;
    const bool deterministic_visual_check = wireframe_check || tetweave_cutaway_check ||
        selected_atlas_check || whole_cell_check || whole_cell_cutaway_check||
        manipulator_visual_check||retained_upload_check;
    const auto initial_subdivision_method=(whole_cell_check||whole_cell_cutaway_check)
        ?tetra::SubdivisionMethod::maubach_diamond
        :tetra_viewer::default_subdivision_method;
    tetra::TetMesh mesh=tetra::TetMesh::make_unit_cube(initial_subdivision_method);
    std::size_t subdivision_method_index=static_cast<std::size_t>(std::distance(
        tetra::subdivision_methods.begin(),
        std::find(tetra::subdivision_methods.begin(),tetra::subdivision_methods.end(),
                  initial_subdivision_method)));
    tetra_viewer::SurfaceMethod surface_method=tetra_viewer::default_surface_method;
    if(wireframe_check||whole_cell_check||whole_cell_cutaway_check)
        surface_method=tetra_viewer::SurfaceMethod::full_tetrahedra;
    if(retained_upload_check)
        surface_method=tetra_viewer::SurfaceMethod::marching_tetrahedra;
    tetra_viewer::VolumeConnectionMethod volume_connection_method =
        (tetweave_cutaway_check || selected_atlas_check)
            ? tetra_viewer::VolumeConnectionMethod::quality_stencils
            : tetra_viewer::default_volume_connection_method;
    if(!deterministic_visual_check)
        volume_connection_method=tetra_viewer::default_volume_connection_for_shape(
            tetra_viewer::default_implicit_shape);
    if(retained_upload_check)
        volume_connection_method=tetra_viewer::VolumeConnectionMethod::hierarchy_cells;
    tetra_viewer::StencilConstruction stencil_construction =
        (tetweave_cutaway_check || selected_atlas_check)
            ? tetra_viewer::StencilConstruction::selected
            : tetra_viewer::StencilConstruction::fixed;
    tetra_viewer::StencilSelectionObjective stencil_selection_objective =
        tetra_viewer::StencilSelectionObjective::balanced;
    tetra_viewer::ShadingModel shading_model = world_mode
        ? tetra_viewer::production_world_profile().shading
        : (selected_atlas_check?tetra_viewer::ShadingModel::dihedral_angle:
                                  tetra_viewer::ShadingModel::studio_flat);
    std::size_t material_rule_index = static_cast<std::size_t>(std::distance(
        tetra_viewer::material_rules.begin(),
        std::find(tetra_viewer::material_rules.begin(),tetra_viewer::material_rules.end(),
                  tetra_viewer::MaterialRule::variational_smooth)));
    if(whole_cell_check||whole_cell_cutaway_check)
        material_rule_index=static_cast<std::size_t>(std::distance(
            tetra_viewer::material_rules.begin(),
            std::find(tetra_viewer::material_rules.begin(),tetra_viewer::material_rules.end(),
                      tetra_viewer::MaterialRule::variational_smooth)));
    bool refined = argc > 1 && strcmp(argv[1], "--refined") == 0;
    const bool sphere_mode = true;
    if (refined)
        mesh.refine_all_binary();
    bool depth_colours = true;
    bool show_camera_lod_zones = false;
    bool show_faces = true;
    bool show_hierarchy_edges = false;
    bool show_surface_edges = deterministic_visual_check;
    bool show_volume_edges = true;
    bool show_volume_faces = true;
    if(retained_upload_check){show_volume_edges=false;show_volume_faces=false;}
    bool x_cutaway=deterministic_visual_check
        ?wireframe_check||tetweave_cutaway_check||whole_cell_cutaway_check
        :true;
    float x_cut_position=deterministic_visual_check?0.5F:1.0F;
    tetra::Sphere sphere{};
    if(!deterministic_visual_check)sphere.kind=tetra_viewer::default_implicit_shape;
    tetra::Camera camera{};
    tetra_viewer::LodCameraPose lod_camera_pose;
    tetra::Vec3 view_camera_position{};
    float sphere_centre[3]{0.5f, 0.5f, 0.5f};
    float sphere_radius = static_cast<float>(sphere.radius);
    tetra_viewer::OrbitCamera orbit_camera;
    if(!deterministic_visual_check){
        orbit_camera.yaw=-0.55;
        orbit_camera.pitch=0.30;
    }
    // Fine enough to produce a visible, discrete interior-tetrahedron volume
    // rather than only a band of surface-intersecting cells.
    float pixel_threshold = 28.0f;
    // Packed root-plus-path addresses support up to 57 refinement steps. Keep the UI
    // comfortably below that representation limit while allowing a useful
    // interactive range for close-up isosurface experiments.
    int maximum_depth = 16;
    tetra::AdaptiveResult last_adaptive_result{};
    bool has_adaptive_result = false;
    double last_refine_milliseconds = -1.0;
    double last_validation_milliseconds = -1.0;
    double last_scene_preparation_milliseconds = -1.0;
    tetra_viewer::SceneCache scene_cache;
    auto geometry_executor=std::make_shared<tetra::GeometryExecutor>(
        tetra::GeometryExecutorConfiguration{
            .worker_count=geometry_worker_count});
    tetra_viewer::ScenePreparationWorker scene_preparation_worker{
        geometry_executor};
    tetra_viewer::PreparedScene background_prepared_scene;
    tetra_viewer::ProjectionStatistics background_projection_statistics;
    std::optional<tetra_viewer::ScenePreparationParameters>
        submitted_scene_preparation;
    std::uint64_t submitted_scene_request_id{};
    std::uint64_t submitted_scene_mesh_revision{
        std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t prepared_scene_mesh_revision{
        std::numeric_limits<std::uint64_t>::max()};
    tetra_viewer::SurfaceDrawChunkStorage surface_draw_chunks;
    tetra_viewer::SurfaceHostStagingStorage surface_host_staging;
    bool retained_surface_upload_ready=false;
    tetra::ImplicitValueCache implicit_value_cache;
    tetra::AdaptationPlanningCache adaptation_planning_cache;
    tetra::FixedFieldSurfaceHierarchy fixed_field_surface_hierarchy;
    tetra::PreorderSurfaceHierarchy preorder_surface_hierarchy;
    tetra::PreorderRenderMetrics preorder_render_metrics;
    std::vector<tetra::TetId> fixed_field_surface_cut;
    std::vector<tetra::Triangle> fixed_field_surface_triangles;
    std::uint64_t fixed_field_surface_cut_revision{};
    tetra::AdaptationConfiguration adaptation_configuration;
    tetra::LayerStorageExperiment interactive_storage_experiment;
    tetra::AdjacencyExperiment interactive_adjacency_experiment;
    std::uint64_t sphere_revision = 0;
    bool upload_dirty = true;
    bool overlay_dirty = true;
    bool retained_upload_present_pending=false;
    enum class CameraDragMode { none, orbit, pan };
    CameraDragMode camera_drag_mode=CameraDragMode::none;
    bool lod_camera_selected=false;
    tetra_viewer::CameraManipulator camera_manipulator;
    tetra_viewer::EmptyViewportGesture empty_viewport_gesture;
    auto& camera_gizmo_mode=camera_manipulator.mode;
    if(manipulator_visual_check){
        lod_camera_selected=true;
        camera_gizmo_mode=manipulator_move_check?tetra_viewer::CameraGizmoMode::translate:
            tetra_viewer::CameraGizmoMode::rotate;
        camera_manipulator.space=manipulator_move_check?
            tetra_viewer::ManipulatorSpace::world:tetra_viewer::ManipulatorSpace::local;
        if(manipulator_move_check)
            camera_manipulator.hovered=tetra_viewer::CameraHandle::move_xy;
        else
            camera_manipulator.active=tetra_viewer::CameraHandle::rotate_y;
        lod_camera_pose.position={0.5,0.9,1.15};
        lod_camera_pose.forward={0.0,-0.5240974256643347,-0.8516583167045438};
        lod_camera_pose.up={0.0,1.0,0.0};
        tetra_viewer::orthonormalize_camera_pose(lod_camera_pose);
        orbit_camera.target=lod_camera_pose.position;
        orbit_camera.distance=2.5;
        orbit_camera.yaw=-0.55;
        orbit_camera.pitch=0.30;
    }
    bool lod_reconcile_pending=false;
    tetra_viewer::MeshUpdateWorker mesh_update_worker{geometry_executor};
    bool mesh_update_in_flight=false;
    std::optional<tetra_viewer::MeshUpdateParameters> submitted_mesh_update;
    tetra_viewer::MeshUpdateOperation submitted_mesh_operation=
        tetra_viewer::MeshUpdateOperation::reconcile_lod;
    std::uint64_t submitted_mesh_request_id{};
    std::uint64_t submitted_mesh_revision{};
    bool lod_reconcile_before_drag=false;
    bool previous_left_pressed=false;
    double previous_cursor_x = 0.0;
    double previous_cursor_y = 0.0;
    if (argc > 1 && strcmp(argv[1], "--sphere-far") == 0) orbit_camera.distance=12.0;
    if (argc > 1 && strcmp(argv[1], "--sphere-fine") == 0) { pixel_threshold = 40.0f; maximum_depth = 3; }
    if (argc > 1 && strcmp(argv[1], "--sphere-offcentre") == 0) {
        sphere_centre[0] = 0.43f; sphere_centre[1] = 0.57f; sphere_centre[2] = 0.46f;
        sphere.centre = {sphere_centre[0], sphere_centre[1], sphere_centre[2]}; sphere.radius = sphere_radius = 0.27f;
        orbit_camera.target=sphere.centre;
    }
    if(!deterministic_visual_check){
        // Keep the editable LOD camera beside the sphere in the initial
        // editor view instead of underneath the floating controls.
        lod_camera_pose.position={0.0,1.0,0.5};
        const auto direction=sphere.centre-lod_camera_pose.position;
        const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                      direction.z*direction.z);
        lod_camera_pose.forward=direction/length;
    }
    if (deterministic_visual_check) {
        if(!whole_cell_check&&!whole_cell_cutaway_check)
            subdivision_method_index = static_cast<std::size_t>(std::distance(
                tetra::subdivision_methods.begin(),
                std::find(tetra::subdivision_methods.begin(),tetra::subdivision_methods.end(),
                          tetra::SubdivisionMethod::bcc_red_green)));
        orbit_camera.distance=0.70;
        orbit_camera.yaw=-0.28;
        orbit_camera.pitch=0.48;
        if(whole_cell_check||whole_cell_cutaway_check)orbit_camera.distance=1.10;
        if(manipulator_visual_check){
            orbit_camera.distance=2.5;
            orbit_camera.yaw=-0.55;
            orbit_camera.pitch=0.30;
            if(manipulator_close_check)orbit_camera.distance=1.4;
            if(manipulator_distant_check)orbit_camera.distance=7.0;
            if(manipulator_edge_on_check){orbit_camera.yaw=0.0;orbit_camera.pitch=0.0;}
        }
    }
    const auto update_orbit_camera = [&] {
        view_camera_position=orbit_camera.position();
    };
    const auto refine_to_current_surface = [&] {
        if (surface_method == tetra_viewer::SurfaceMethod::full_tetrahedra &&
            tetra_viewer::is_variational_material_rule(tetra_viewer::material_rules[material_rule_index]))
            return tetra::refine_to_whole_cell_surface(
                mesh, sphere, camera, pixel_threshold, static_cast<unsigned int>(maximum_depth),
                tetra_viewer::whole_cell_options(tetra_viewer::material_rules[material_rule_index]));
        const double threshold=mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green
            ?static_cast<double>(pixel_threshold)*adaptation_configuration.split_hysteresis
            :static_cast<double>(pixel_threshold);
        return tetra::refine_to_sphere(
            mesh, sphere, camera, threshold, static_cast<unsigned int>(maximum_depth),
            &implicit_value_cache);
    };
    bool reconcile_complete=true;
    const auto reconcile_to_current_surface=[&] {
        reconcile_complete=true;
        if(adaptation_configuration.lod_update==
           tetra::LodUpdateStrategy::full_rebuild_oracle){
            mesh.reset_active_hierarchy();
            return refine_to_current_surface();
        }
        tetra::AdaptiveResult result;
        if(adaptation_configuration.lod_update==
               tetra::LodUpdateStrategy::relevant_surface_hierarchy||
           adaptation_configuration.lod_update==
               tetra::LodUpdateStrategy::minimal_surface_hierarchy||
           adaptation_configuration.lod_update==
               tetra::LodUpdateStrategy::on_demand_render_traversal){
            static_cast<void>(tetra::update_fixed_field_surface_hierarchy(
                fixed_field_surface_hierarchy,mesh,sphere,sphere_revision));
            if(adaptation_configuration.lod_update==
               tetra::LodUpdateStrategy::on_demand_render_traversal){
                static_cast<void>(tetra::update_preorder_surface_hierarchy(
                    preorder_surface_hierarchy,fixed_field_surface_hierarchy));
                fixed_field_surface_cut.clear();
                preorder_render_metrics=tetra::render_preorder_surface(
                    preorder_surface_hierarchy,mesh,sphere,camera,pixel_threshold,
                    static_cast<unsigned int>(maximum_depth),
                    fixed_field_surface_triangles);
            }else{
                fixed_field_surface_cut=tetra::select_fixed_field_surface_cut(
                    fixed_field_surface_hierarchy,mesh,camera,pixel_threshold,
                    static_cast<unsigned int>(maximum_depth),
                    adaptation_configuration.lod_update);
                fixed_field_surface_triangles=tetra::extract_isosurface(
                    mesh,sphere,fixed_field_surface_cut);
            }
            ++fixed_field_surface_cut_revision;
            return result;
        }
        if(!fixed_field_surface_triangles.empty()){
            fixed_field_surface_cut.clear();
            fixed_field_surface_triangles.clear();
            ++fixed_field_surface_cut_revision;
        }
        if(mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green){
            const auto commit=tetra::adapt_to_surface(
                mesh,sphere,camera,pixel_threshold,
                static_cast<unsigned int>(maximum_depth),adaptation_configuration,
                sphere_revision,&adaptation_planning_cache);
            if(commit.status==tetra::AdaptationCommitStatus::committed){
                ++result.iterations;
                result.refined_leaves+=commit.accepted_splits;
                reconcile_complete=false;
                return result;
            }
            if(commit.status!=tetra::AdaptationCommitStatus::no_change){
                result.reached_depth_limit=true;
                return result;
            }
            return result;
        }
        const auto completion=refine_to_current_surface();
        result.iterations+=completion.iterations;
        result.refined_leaves+=completion.refined_leaves;
        result.reached_depth_limit|=completion.reached_depth_limit;
        return result;
    };
    update_orbit_camera();
    lod_camera_pose.apply(camera);
    if (sphere_mode&&!world_mode) {
        last_adaptive_result = refine_to_current_surface();
        has_adaptive_result = true;
        refined = true;
    }
    const auto validate_mesh = [&] {
        // Conformity validation already includes the adjacency check.
        return mesh.has_positive_active_volumes() && mesh.has_conforming_active_faces();
    };
    bool mesh_valid = validate_mesh();
    bool mesh_validation_current = true;
    ImVec4 clear_color = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    const auto mesh_update_parameters=[&](
        tetra_viewer::MeshUpdateIntent intent=
            tetra_viewer::MeshUpdateIntent::settled) {
        tetra_viewer::MeshUpdateParameters parameters{
            sphere,camera,static_cast<double>(pixel_threshold),
            static_cast<unsigned int>(maximum_depth),adaptation_configuration,
            sphere_revision,{.target_milliseconds=
                tetra_viewer::default_mesh_update_time_budget_milliseconds}};
        if(intent==tetra_viewer::MeshUpdateIntent::interactive_camera)
            return tetra_viewer::make_interactive_mesh_update_parameters(
                std::move(parameters));
        return parameters;
    };
    tetra_viewer::FirstPersonController world_controller;
    tetra::Camera world_lod_camera;
    std::unique_ptr<tetra_viewer::TerrainRuntime> world_runtime;
    std::future<std::unique_ptr<tetra_viewer::TerrainRuntime>>
        world_runtime_startup;
    std::uint64_t world_scene_generation{};
    bool world_pointer_captured=world_mode;
    bool world_paused=false;
    bool world_single_step=false;
    bool world_free_fly=false;
    bool world_lock_lod_camera=true;
    bool world_show_capsule=false;
    bool world_show_contact_normal=false;
    bool world_smooth_normals=false;
    bool world_terrain_msaa=false;
    float world_sun_azimuth=
        tetra_viewer::default_world_sun_azimuth_radians;
    float world_sun_elevation=
        tetra_viewer::default_world_sun_elevation_radians;
    bool world_animate_sun=false;
    double world_sun_cycle_seconds=
        tetra_viewer::default_world_sun_cycle_seconds;
    double world_sun_orbit_azimuth=world_sun_azimuth;
    double world_sun_orbit_phase=world_sun_elevation;
    tetra_viewer::AtmospherePreset world_atmosphere_preset=
        tetra_viewer::default_world_atmosphere_preset;
    bool world_atmosphere_rendering_method_explicit=false;
    double world_exposure_ev=-0.62;
    bool world_gpu_atmosphere_benchmark=false;
    bool world_gpu_atmosphere_probe=false;
    bool world_gpu_shadow_projection_probe=false;
    std::string world_gpu_atmosphere_capture_path;
    bool world_gpu_atmosphere_capture_submitted=false;
    std::size_t world_gpu_capture_ready_frames{};
    std::size_t world_gpu_capture_frame_target{};
    std::size_t world_gpu_rendered_frames{};
    std::size_t world_gpu_capture_after_motion_frames{};
    std::size_t world_gpu_capture_motion_frame_count{};
    bool world_gpu_atmosphere_resize_check=false;
    std::size_t world_gpu_walk_steps{};
    std::size_t world_gpu_walk_steps_remaining{};
    double world_gpu_look_x{},world_gpu_look_y{};
    std::size_t world_gpu_look_frames{1U};
    std::size_t world_gpu_look_frames_remaining{};
    bool world_gpu_motion_applied=false;
    bool world_gpu_motion_saw_busy=false;
    bool world_analytic_ridge=false;
    std::optional<tetra_viewer::AtmosphereShadowFrontRequest>
        world_retained_atmosphere_shadow_request;
    bool world_gpu_atmosphere_resize_requested=false;
    bool world_gpu_automation_requested=false;
    double world_maximum_terrain_relief_metres{};
    std::size_t world_gpu_benchmark_warmup_frames{};
    std::array<std::vector<double>,7> world_gpu_benchmark_samples;
    std::array<double,7> world_gpu_refresh_maximum{};
    std::size_t world_gpu_pre_resize_scene_bytes{};
    int world_process_exit_code{};
    double world_cursor_x{},world_cursor_y{};
    auto previous_world_frame=std::chrono::steady_clock::now();
    if(world_mode){
        constexpr std::string_view preset_prefix="--atmosphere-preset=";
        constexpr std::string_view azimuth_prefix="--sun-azimuth-degrees=";
        constexpr std::string_view elevation_prefix="--sun-elevation-degrees=";
        constexpr std::string_view sun_cycle_prefix="--sun-cycle-seconds=";
        constexpr std::string_view exposure_prefix="--exposure-ev=";
        constexpr std::string_view debug_prefix="--atmosphere-debug=";
        constexpr std::string_view quality_prefix="--atmosphere-quality=";
        constexpr std::string_view transport_prefix="--atmosphere-transport=";
        constexpr std::string_view rendering_method_prefix=
            "--atmosphere-rendering-method=";
        constexpr std::string_view screen_resolution_prefix=
            "--atmosphere-screen-resolution-divisor=";
        constexpr std::string_view shadow_integrator_prefix=
            "--atmosphere-shadow-integrator=";
        constexpr std::string_view surface_bias_prefix=
            "--surface-shadow-bias=";
        constexpr std::string_view shadow_filter_prefix=
            "--atmosphere-shadow-filter=";
        constexpr std::string_view raster_constant_prefix=
            "--shadow-raster-constant=";
        constexpr std::string_view raster_slope_prefix=
            "--shadow-raster-slope=";
        constexpr std::string_view comparison_bias_prefix=
            "--atmosphere-comparison-bias-metres=";
        constexpr std::string_view comparison_bias_world_prefix=
            "--atmosphere-comparison-bias-world=";
        constexpr std::string_view capture_prefix="--gpu-atmosphere-capture=";
        constexpr std::string_view capture_after_motion_prefix=
            "--gpu-atmosphere-capture-after-motion-frames=";
        constexpr std::string_view capture_frame_prefix=
            "--gpu-atmosphere-capture-frame=";
        constexpr std::string_view camera_prefix="--camera-feet=";
        constexpr std::string_view yaw_prefix="--camera-yaw-degrees=";
        constexpr std::string_view pitch_prefix="--camera-pitch-degrees=";
        constexpr std::string_view walk_prefix="--automation-walk-steps=";
        constexpr std::string_view look_prefix="--automation-look=";
        constexpr std::string_view look_frames_prefix=
            "--automation-look-frames=";
        const auto parse_argument_double=[&](std::string_view text,
                                             double& destination){
            const std::string value(text);
            char* end=nullptr;
            const double parsed=std::strtod(value.c_str(),&end);
            if(end==value.c_str()||*end!='\0'||!std::isfinite(parsed))return false;
            destination=parsed;
            return true;
        };
        const auto parse_camera_position=[&](std::string_view text){
            std::array<double,3> values{};
            for(std::size_t axis=0;axis<values.size();++axis){
                const auto separator=text.find(',');
                const auto component=separator==std::string_view::npos?
                    text:text.substr(0,separator);
                if(!parse_argument_double(component,values[axis]))return false;
                if(axis+1U<values.size()){
                    if(separator==std::string_view::npos)return false;
                    text.remove_prefix(separator+1U);
                }else if(separator!=std::string_view::npos)return false;
            }
            world_controller.state().feet={values[0],values[1],values[2]};
            return true;
        };
        for(int argument=1;argument<argc;++argument){
            const std::string_view value=argv[argument];
            if(value=="--atmosphere-off"){
                g_AtmosphereFrame.enabled=false;
            }else if(value=="--analytic-ridge"){
                world_analytic_ridge=true;
            }else if(value=="--surface-edges-off"){
                show_surface_edges=false;
            }else if(value=="--smooth-terrain-normals"){
                world_smooth_normals=true;
            }else if(value=="--terrain-msaa"){
                world_terrain_msaa=true;
            }else if(value=="--gpu-atmosphere-benchmark"){
                world_gpu_atmosphere_benchmark=true;
            }else if(value=="--gpu-atmosphere-resize-check"){
                world_gpu_atmosphere_benchmark=true;
                world_gpu_atmosphere_resize_check=true;
            }else if(value=="--gpu-atmosphere-probe"){
                world_gpu_atmosphere_probe=true;
                g_AtmosphereFrame.numeric_probe_requested=true;
            }else if(value=="--gpu-shadow-projection-probe"){
                world_gpu_shadow_projection_probe=true;
                g_AtmosphereFrame.shadow_projection_probe_requested=true;
            }else if(value.starts_with(capture_prefix)){
                world_gpu_atmosphere_capture_path=
                    std::string{value.substr(capture_prefix.size())};
                if(world_gpu_atmosphere_capture_path.empty()){
                    fprintf(stderr,"GPU atmosphere capture path is empty\n");
                    return 2;
                }
            }else if(value.starts_with(capture_after_motion_prefix)){
                const auto text=value.substr(capture_after_motion_prefix.size());
                std::size_t parsed{};
                const auto [end,error]=std::from_chars(
                    text.data(),text.data()+text.size(),parsed);
                if(error!=std::errc{}||end!=text.data()+text.size()||
                   parsed==0U||parsed>10000U){
                    fprintf(stderr,
                        "capture after motion frames must be in [1,10000]\n");
                    return 2;
                }
                world_gpu_capture_after_motion_frames=parsed;
            }else if(value.starts_with(capture_frame_prefix)){
                const auto text=value.substr(capture_frame_prefix.size());
                std::size_t parsed{};
                const auto [end,error]=std::from_chars(
                    text.data(),text.data()+text.size(),parsed);
                if(error!=std::errc{}||end!=text.data()+text.size()||
                   parsed==0U||parsed>10000U){
                    fprintf(stderr,"capture frame must be in [1,10000]\n");
                    return 2;
                }
                world_gpu_capture_frame_target=parsed;
            }else if(value=="--free-fly"){
                world_free_fly=true;
            }else if(value=="--terrain-lod-follow"){
                world_lock_lod_camera=false;
            }else if(value.starts_with(camera_prefix)){
                if(!parse_camera_position(value.substr(camera_prefix.size()))){
                    fprintf(stderr,"camera feet must be finite x,y,z values\n");
                    return 2;
                }
            }else if(value.starts_with(walk_prefix)){
                const auto text=value.substr(walk_prefix.size());
                std::size_t parsed{};
                const auto [end,error]=std::from_chars(
                    text.data(),text.data()+text.size(),parsed);
                if(error!=std::errc{}||end!=text.data()+text.size()||
                   parsed>10000U){
                    fprintf(stderr,"automation walk steps must be in [0,10000]\n");
                    return 2;
                }
                world_gpu_walk_steps=parsed;
                world_gpu_walk_steps_remaining=parsed;
            }else if(value.starts_with(look_frames_prefix)){
                const auto text=value.substr(look_frames_prefix.size());
                std::size_t parsed{};
                const auto [end,error]=std::from_chars(
                    text.data(),text.data()+text.size(),parsed);
                if(error!=std::errc{}||end!=text.data()+text.size()||
                   parsed==0U||parsed>10000U){
                    fprintf(stderr,"automation look frames must be in [1,10000]\n");
                    return 2;
                }
                world_gpu_look_frames=parsed;
            }else if(value.starts_with(look_prefix)){
                auto text=value.substr(look_prefix.size());
                const auto comma=text.find(',');
                if(comma==std::string_view::npos||
                   !parse_argument_double(text.substr(0,comma),world_gpu_look_x)||
                   !parse_argument_double(text.substr(comma+1U),world_gpu_look_y)){
                    fprintf(stderr,"automation look must be finite DX,DY\n");
                    return 2;
                }
            }else if(value.starts_with(yaw_prefix)){
                double degrees{};
                if(!parse_argument_double(value.substr(yaw_prefix.size()),degrees)){
                    fprintf(stderr,"camera yaw must be finite\n");return 2;
                }
                world_controller.state().yaw=degrees*std::numbers::pi/180.0;
            }else if(value.starts_with(pitch_prefix)){
                double degrees{};
                if(!parse_argument_double(value.substr(pitch_prefix.size()),degrees)){
                    fprintf(stderr,"camera pitch must be finite\n");return 2;
                }
                world_controller.state().pitch=std::clamp(
                    degrees*std::numbers::pi/180.0,-1.55,1.55);
            }else if(value.starts_with(preset_prefix)){
                const auto preset=tetra_viewer::parse_atmosphere_preset(
                    value.substr(preset_prefix.size()));
                if(!preset||*preset==tetra_viewer::AtmospherePreset::custom){
                    fprintf(stderr,"unknown launch atmosphere preset\n");
                    return 2;
                }
                const double scale=
                    g_AtmosphereFrame.parameters.metres_per_world_unit;
                g_AtmosphereFrame.parameters=
                    tetra_viewer::atmosphere_preset(*preset);
                g_AtmosphereFrame.parameters.metres_per_world_unit=scale;
                world_atmosphere_preset=*preset;
            }else if(value.starts_with(azimuth_prefix)){
                double degrees{};
                if(!parse_argument_double(value.substr(azimuth_prefix.size()),
                                          degrees)){
                    fprintf(stderr,"sun azimuth must be finite\n");return 2;
                }
                world_sun_azimuth=static_cast<float>(degrees*
                    std::numbers::pi/180.0);
            }else if(value.starts_with(elevation_prefix)){
                double degrees{};
                if(!parse_argument_double(value.substr(elevation_prefix.size()),
                                          degrees)){
                    fprintf(stderr,"sun elevation must be finite\n");return 2;
                }
                world_sun_elevation=static_cast<float>(degrees*
                    std::numbers::pi/180.0);
            }else if(value.starts_with(sun_cycle_prefix)){
                double seconds{};
                if(!parse_argument_double(
                       value.substr(sun_cycle_prefix.size()),seconds)||
                   seconds<0.25||seconds>600.0){
                    fprintf(stderr,"sun cycle must be in [0.25,600] seconds\n");
                    return 2;
                }
                world_sun_cycle_seconds=seconds;
                world_animate_sun=true;
            }else if(value.starts_with(exposure_prefix)){
                if(!parse_argument_double(value.substr(exposure_prefix.size()),
                                          world_exposure_ev)){
                    fprintf(stderr,"exposure must be finite\n");return 2;
                }
                world_exposure_ev=std::clamp(world_exposure_ev,-6.0,6.0);
            }else if(value.starts_with(debug_prefix)){
                const std::string debug(value.substr(debug_prefix.size()));
                char* end=nullptr;
                const long parsed=std::strtol(debug.c_str(),&end,10);
                if(debug.empty()||end==nullptr||*end!='\0'||parsed<0||parsed>27){
                    fprintf(stderr,"atmosphere debug view must be in [0,27]\n");
                    return 2;
                }
                g_AtmosphereFrame.debug_view=static_cast<int>(parsed);
            }else if(value.starts_with(quality_prefix)){
                const auto quality=value.substr(quality_prefix.size());
                if(quality=="low")
                    g_AtmosphereFrame.quality=
                        tetra_viewer::AtmosphereQuality::low;
                else if(quality=="default")
                    g_AtmosphereFrame.quality=
                        tetra_viewer::AtmosphereQuality::standard;
                else if(quality=="high")
                    g_AtmosphereFrame.quality=
                        tetra_viewer::AtmosphereQuality::high;
                else{
                    fprintf(stderr,"unknown atmosphere quality\n");
                    return 2;
                }
            }else if(value.starts_with(transport_prefix)){
                const auto transport=tetra_viewer::parse_atmosphere_transport(
                    value.substr(transport_prefix.size()));
                if(!transport){
                    fprintf(stderr,"unknown atmosphere transport\n");
                    return 2;
                }
                g_AtmosphereFrame.transport=*transport;
            }else if(value.starts_with(rendering_method_prefix)){
                const auto method=
                    tetra_viewer::parse_atmosphere_rendering_method(
                        value.substr(rendering_method_prefix.size()));
                if(!method){
                    fprintf(stderr,"unknown atmosphere rendering method\n");
                    return 2;
                }
                g_AtmosphereFrame.rendering_method=*method;
                world_atmosphere_rendering_method_explicit=true;
            }else if(value.starts_with(screen_resolution_prefix)){
                const std::string divisor_text(
                    value.substr(screen_resolution_prefix.size()));
                char* end=nullptr;
                const long divisor=std::strtol(
                    divisor_text.c_str(),&end,10);
                if(divisor_text.empty()||end==nullptr||*end!='\0'||
                   divisor<2||divisor>4){
                    fprintf(stderr,
                        "atmosphere screen resolution divisor must be in [2,4]\n");
                    return 2;
                }
                g_AtmosphereFrame.screen_resolution_divisor=
                    static_cast<std::uint32_t>(divisor);
            }else if(value.starts_with(shadow_integrator_prefix)){
                const auto integrator=
                    tetra_viewer::parse_atmosphere_shadow_integrator(
                        value.substr(shadow_integrator_prefix.size()));
                if(!integrator){
                    fprintf(stderr,"unknown atmosphere shadow integrator\n");
                    return 2;
                }
                g_AtmosphereFrame.shadow_integrator=*integrator;
            }else if(value.starts_with(surface_bias_prefix)){
                const auto mode=tetra_viewer::parse_surface_shadow_bias_mode(
                    value.substr(surface_bias_prefix.size()));
                if(!mode){
                    fprintf(stderr,"unknown surface shadow bias mode\n");
                    return 2;
                }
                g_AtmosphereFrame.surface_shadow_bias=*mode;
            }else if(value.starts_with(shadow_filter_prefix)){
                const auto filter=tetra_viewer::parse_atmosphere_shadow_filter(
                    value.substr(shadow_filter_prefix.size()));
                if(!filter){
                    fprintf(stderr,"unknown atmosphere shadow filter\n");
                    return 2;
                }
                g_AtmosphereFrame.shadow_filter=*filter;
            }else if(value.starts_with(raster_constant_prefix)){
                double parsed{};
                if(!parse_argument_double(
                    value.substr(raster_constant_prefix.size()),parsed)||
                   parsed<0.0||parsed>16.0){
                    fprintf(stderr,"invalid shadow raster constant bias\n");
                    return 2;
                }
                g_AtmosphereFrame.shadow_raster_bias_constant=
                    static_cast<float>(parsed);
            }else if(value.starts_with(raster_slope_prefix)){
                double parsed{};
                if(!parse_argument_double(
                    value.substr(raster_slope_prefix.size()),parsed)||
                   parsed<0.0||parsed>16.0){
                    fprintf(stderr,"invalid shadow raster slope bias\n");
                    return 2;
                }
                g_AtmosphereFrame.shadow_raster_bias_slope=
                    static_cast<float>(parsed);
            }else if(value.starts_with(comparison_bias_prefix)){
                double parsed{};
                if(!parse_argument_double(
                    value.substr(comparison_bias_prefix.size()),parsed)||
                   parsed<0.0||parsed>0.1){
                    fprintf(stderr,"invalid atmosphere comparison bias\n");
                    return 2;
                }
                g_AtmosphereFrame.
                    atmosphere_shadow_comparison_bias_world_override=parsed;
            }else if(value.starts_with(comparison_bias_world_prefix)){
                double parsed{};
                if(!parse_argument_double(
                    value.substr(comparison_bias_world_prefix.size()),parsed)||
                   parsed<0.0||parsed>0.1){
                    fprintf(stderr,"invalid atmosphere comparison bias\n");
                    return 2;
                }
                g_AtmosphereFrame.
                    atmosphere_shadow_comparison_bias_world_override=parsed;
            }
        }
        if(g_AtmosphereFrame.transport==
               tetra_viewer::AtmosphereTransport::reference_hillaire_2020&&
           !world_atmosphere_rendering_method_explicit)
            g_AtmosphereFrame.rendering_method=
                tetra_viewer::AtmosphereRenderingMethod::
                    temporal_half_resolution;
        world_sun_orbit_azimuth=world_sun_azimuth;
        world_sun_orbit_phase=world_sun_elevation;
        world_gpu_automation_requested=world_gpu_atmosphere_benchmark||
            world_gpu_atmosphere_probe||
            world_gpu_shadow_projection_probe||
            !world_gpu_atmosphere_capture_path.empty()||
            world_gpu_walk_steps!=0U||world_gpu_look_x!=0.0||
            world_gpu_look_y!=0.0;
        if(g_AtmosphereFrame.shadow_integrator==
               tetra_viewer::AtmosphereShadowIntegrator::dense_oracle&&
           !world_gpu_automation_requested){
            fprintf(stderr,"dense-oracle atmosphere shadow integration is headless only\n");
            return 2;
        }
        if(world_terrain_msaa&&!g_SceneRenderer.supports_terrain_msaa()){
            fprintf(stderr,"4x terrain MSAA is unavailable on this device\n");
            return 2;
        }
        if(g_AtmosphereFrame.quality!=
               tetra_viewer::AtmosphereQuality::standard||
           g_AtmosphereFrame.screen_resolution_divisor!=4U||
           world_terrain_msaa){
            g_SceneRenderer.recreate(
                {static_cast<std::uint32_t>(g_MainWindowData.Width),
                 static_cast<std::uint32_t>(g_MainWindowData.Height)},
                g_MainWindowData.ImageCount,g_AtmosphereFrame.quality,
                g_AtmosphereFrame.screen_resolution_divisor,
                world_terrain_msaa);
        }
        if(world_gpu_atmosphere_probe&&g_AtmosphereFrame.transport!=
           tetra_viewer::AtmosphereTransport::faithful_hillaire){
            fprintf(stderr,"--gpu-atmosphere-probe requires "
                           "--atmosphere-transport=faithful-hillaire\n");
            return 2;
        }
        if(world_gpu_atmosphere_probe&&world_gpu_shadow_projection_probe){
            fprintf(stderr,"GPU atmosphere probes are mutually exclusive\n");
            return 2;
        }
    }
    if(world_mode){
        auto profile=tetra_viewer::production_world_profile();
        profile.terrain.analytic_ridge=world_analytic_ridge;
        if(world_analytic_ridge){
            profile.view_distance=12.0;
            profile.pixel_threshold=48.0;
            profile.budgets.maximum_cpu_bytes=768U*1024U*1024U;
            profile.budgets.maximum_upload_bytes=64U*1024U*1024U;
        }
        g_AtmosphereFrame.minimum_analytic_ground_distance_metres=
            profile.view_distance*
            g_AtmosphereFrame.parameters.metres_per_world_unit*0.9;
        sphere.kind=profile.shape;sphere.terrain=profile.terrain;
        sphere.secondary=profile.octave_detail_amplitude;
        sphere.frequency=profile.octave_detail_frequency;
        world_maximum_terrain_relief_metres=
            tetra::terrain_height_magnitude_bound(sphere)*
            g_AtmosphereFrame.parameters.metres_per_world_unit;
        if(world_atmosphere_preset==
           tetra_viewer::AtmospherePreset::gameplay_planet)
            g_AtmosphereFrame.parameters=
                tetra_viewer::adapt_compact_atmosphere_to_relief(
                    g_AtmosphereFrame.parameters,
                    world_maximum_terrain_relief_metres);
        const auto framebuffer_aspect=static_cast<double>(std::max(w,1))/
            static_cast<double>(std::max(h,1));
        camera=world_controller.camera(std::max(h,1),framebuffer_aspect);
        world_lod_camera=camera;
        world_runtime_startup=
            tetra_viewer::make_production_terrain_runtime_async(profile);
        world_pointer_captured=!world_gpu_automation_requested;
        glfwSetInputMode(window,GLFW_CURSOR,world_pointer_captured?
            GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);
        glfwGetCursorPos(window,&world_cursor_x,&world_cursor_y);
        view_camera_position=world_controller.eye_position();
        depth_colours=false;
        show_volume_edges=false;
        show_volume_faces=false;
        x_cutaway=false;
    }

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        // Publication is the only point where the render-thread mesh changes.
        // The worker owns and mutates a private snapshot while this thread
        // continues presenting the previous complete scene.
        bool mesh_slice_published_this_frame=false;
        if(world_mode){
            if(!world_runtime&&world_runtime_startup.valid()&&
               world_runtime_startup.wait_for(std::chrono::seconds(0))==
                   std::future_status::ready){
                world_runtime=world_runtime_startup.get();
                sphere=world_runtime->field();
                world_runtime->set_camera(world_lod_camera,false);
            }
            g_BlackSceneClear=!world_runtime;
            if(world_runtime&&world_runtime->update()){
                sphere=world_runtime->field();
                mesh_slice_published_this_frame=true;
                mesh_validation_current=false;
            }
            const auto runtime_status=world_runtime?
                world_runtime->diagnostics():tetra_viewer::TerrainRuntimeDiagnostics{};
            const bool scripted_motion=world_gpu_walk_steps!=0U||
                world_gpu_look_x!=0.0||world_gpu_look_y!=0.0;
            if(scripted_motion&&!world_gpu_motion_applied&&world_runtime&&
               !runtime_status.busy&&world_gpu_capture_ready_frames>=64U&&
               g_SceneRenderer.latest_atmosphere_lookup_revisions()){
                world_gpu_motion_applied=true;
                world_gpu_look_frames_remaining=world_gpu_look_frames;
            }
            if(world_gpu_motion_applied&&world_gpu_look_frames_remaining!=0U){
                world_controller.look(
                    world_gpu_look_x/static_cast<double>(world_gpu_look_frames),
                    world_gpu_look_y/static_cast<double>(world_gpu_look_frames));
                --world_gpu_look_frames_remaining;
            }
            if(world_gpu_motion_applied&&runtime_status.busy)
                world_gpu_motion_saw_busy=true;
            if(world_gpu_motion_applied)
                ++world_gpu_capture_motion_frame_count;
            if(world_runtime&&
               runtime_status.scene_generation!=world_scene_generation){
                if(world_runtime->retained_surface()!=nullptr){
                    background_prepared_scene={};
                    background_prepared_scene.render_origin=
                        world_runtime->render_origin();
                }else background_prepared_scene=world_runtime->scene();
                prepared_scene_mesh_revision=runtime_status.scene_mesh_revision;
                world_scene_generation=runtime_status.scene_generation;
                upload_dirty=true;
            }
        }
        if(!world_mode)if(auto completed=mesh_update_worker.take_completed()){
            const auto current_intent=camera_manipulator.dragging()
                ?tetra_viewer::MeshUpdateIntent::interactive_camera
                :tetra_viewer::MeshUpdateIntent::settled;
            const auto current_parameters=mesh_update_parameters(current_intent);
            const bool camera_pose_advanced=submitted_mesh_update&&
                submitted_mesh_update->intent==
                    tetra_viewer::MeshUpdateIntent::interactive_camera&&
                tetra_viewer::compatible_mesh_update_publication(
                    *submitted_mesh_update,current_parameters)&&
                !tetra_viewer::same_mesh_update_parameters(
                    *submitted_mesh_update,current_parameters);
            const auto publication=tetra_viewer::publish_mesh_update_result(
                mesh_update_worker,std::move(*completed),mesh,
                adaptation_planning_cache,submitted_mesh_request_id,
                submitted_mesh_operation,current_parameters);
            if(publication.published()){
                mesh_slice_published_this_frame=true;
                last_adaptive_result=publication.adaptation;
                last_refine_milliseconds=publication.duration_milliseconds;
                has_adaptive_result=true;
                refined=true;
                mesh_validation_current=false;
                if(retained_upload_check)upload_dirty=true;
                submitted_mesh_revision=mesh.revision();
                if(publication.status==
                   tetra_viewer::MeshPublicationStatus::intermediate){
                    submitted_mesh_request_id=publication.request_id;
                    mesh_update_in_flight=true;
                    lod_reconcile_pending=true;
                }else{
                    mesh_update_in_flight=false;
                    submitted_mesh_update.reset();
                    submitted_mesh_request_id=0U;
                    lod_reconcile_pending=camera_pose_advanced;
                }
            }else{
                // A camera, field, setting, or direct mesh edit superseded the
                // snapshot. Never publish stale geometry.
                mesh_update_in_flight=false;
                submitted_mesh_update.reset();
                submitted_mesh_request_id=0U;
                lod_reconcile_pending=true;
            }
        }

        // Resize swap chain?
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
            g_SceneRenderer.recreate(
                {static_cast<std::uint32_t>(g_MainWindowData.Width),
                 static_cast<std::uint32_t>(g_MainWindowData.Height)},
                g_MainWindowData.ImageCount,g_AtmosphereFrame.quality,
                g_AtmosphereFrame.screen_resolution_divisor,
                world_terrain_msaa);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // GLFW continues reporting a synthetic cursor position while disabled.
        // Do not feed that position or any captured clicks to Dear ImGui: in
        // first-person mode the mouse belongs exclusively to camera look.
        // Keyboard navigation remains enabled so the documented hotkeys work.
        auto& frame_input=ImGui::GetIO();
        if(world_mode&&!tetra_viewer::world_ui_accepts_pointer(
                world_pointer_captured))
            frame_input.ConfigFlags|=ImGuiConfigFlags_NoMouse;
        else
            frame_input.ConfigFlags&=~ImGuiConfigFlags_NoMouse;

        // Start the Dear ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Submit the canvas first.  The normal Dear ImGui controls created
        // below are therefore a genuine floating overlay in front of it.
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::Begin("Tetrahedral viewport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        ImGui::End();

        ImGui::SetNextWindowPos(world_mode?ImVec2(-10000.0F,-10000.0F):
            (manipulator_panel_check?ImVec2(350.0f,220.0f):ImVec2(16.0f,16.0f)),
            world_mode||manipulator_panel_check?ImGuiCond_Always:ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(300.0f, 0.0f),
            ImVec2(340.0f, std::max(240.0f, ImGui::GetIO().DisplaySize.y - 32.0f)));
        ImGui::SetNextWindowBgAlpha(1.0F);
        ImGui::Begin("Adaptive sphere controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextWrapped("Adaptive tetrahedra against an implicit sphere");
        ImGui::SeparatorText("Geometry");
        const auto current_method = tetra::subdivision_methods[subdivision_method_index];
        const auto current_method_name = tetra::subdivision_method_name(current_method);
        ImGui::TextDisabled("Subdivision method");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##Subdivision method", current_method_name.data())) {
            for (std::size_t method_index = 0; method_index < tetra::subdivision_methods.size(); ++method_index) {
                const auto method = tetra::subdivision_methods[method_index];
                const bool selected = method_index == subdivision_method_index;
                ImGui::PushID(static_cast<int>(method_index));
                if (ImGui::Selectable(tetra::subdivision_method_name(method).data(), selected) && !selected) {
                    subdivision_method_index = method_index;
                    mesh = tetra::TetMesh::make_unit_cube(method);
                    if(method==tetra::SubdivisionMethod::bcc_red_green)
                        static_cast<void>(mesh.set_transition_strategy(
                            adaptation_configuration.transition_strategy));
                    implicit_value_cache.clear();
                    const auto start = std::chrono::steady_clock::now();
                    last_adaptive_result = refine_to_current_surface();
                    last_refine_milliseconds = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                    has_adaptive_result = true;
                    refined = true;
                    mesh_validation_current = false;
                }
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        const auto resolved_volume_connection=
            tetra_viewer::resolve_interactive_volume_connection(
                surface_method,volume_connection_method,x_cutaway&&show_volume_faces);
        const bool resolved_disconnected_cutaway=resolved_volume_connection!=volume_connection_method;
        volume_connection_method=resolved_volume_connection;
        const bool connected_cutaway_surface =
            tetra_viewer::uses_connected_volume(volume_connection_method) &&
            x_cutaway && show_volume_faces;
        ImGui::TextDisabled(connected_cutaway_surface ? "Connected volume boundary" : "Surface method");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto displayed_surface_name=connected_cutaway_surface &&
            !tetra_viewer::supports_connected_volume(surface_method)
            ? std::string_view{"Connected cleaved boundary"}
            : tetra_viewer::surface_method_name(surface_method);
        if (ImGui::BeginCombo("##Surface method", displayed_surface_name.data())) {
            for (std::size_t method_index = 0; method_index < tetra_viewer::surface_methods.size(); ++method_index) {
                const auto candidate = tetra_viewer::surface_methods[method_index];
                const bool selected = candidate == surface_method;
                const bool fixed_shell_surface=volume_connection_method==
                    tetra_viewer::VolumeConnectionMethod::fixed_surface_shell;
                const bool available = (!connected_cutaway_surface ||
                    tetra_viewer::supports_connected_volume(candidate))&&
                    (!fixed_shell_surface||candidate==tetra_viewer::SurfaceMethod::surface_optimization);
                ImGui::PushID(static_cast<int>(100 + method_index));
                ImGui::BeginDisabled(!available);
                if (ImGui::Selectable(tetra_viewer::surface_method_name(candidate).data(), selected))
                    surface_method = candidate;
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Volume connection");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##Volume connection",
                              tetra_viewer::volume_connection_method_name(volume_connection_method).data())) {
            for (std::size_t method_index = 0;
                 method_index < tetra_viewer::volume_connection_methods.size(); ++method_index) {
                const auto candidate = tetra_viewer::volume_connection_methods[method_index];
                const bool selected = candidate == volume_connection_method;
                const bool available=tetra_viewer::volume_connection_available(
                    surface_method,candidate);
                ImGui::PushID(static_cast<int>(150 + method_index));
                ImGui::BeginDisabled(!available);
                if (ImGui::Selectable(tetra_viewer::volume_connection_method_name(candidate).data(), selected))
                    volume_connection_method = candidate;
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (connected_cutaway_surface)
            ImGui::TextWrapped("Only methods that share the connected volume boundary are available. Surface optimization moves those boundary vertices while preserving valid tetrahedra.");
        if(volume_connection_method==tetra_viewer::VolumeConnectionMethod::hierarchy_cells&&
           surface_method==tetra_viewer::SurfaceMethod::surface_optimization)
            ImGui::TextColored(ImVec4(1.0f,0.72f,0.28f,1.0f),
                "Disconnected comparison: the optimized surface and whole-cell boundary are independent, so a solid cutaway can expose a gap. Use Connected hierarchy core for one watertight complex.");
        if(resolved_disconnected_cutaway)
            ImGui::TextColored(ImVec4(1.0f,0.72f,0.28f,1.0f),
                               "Selected the fixed surface shell to prevent cracks in this cutaway.");
        if (volume_connection_method == tetra_viewer::VolumeConnectionMethod::coned_prototype)
            ImGui::TextWrapped("Comparison baseline: cones every clipped polyhedron to a centre point. This is robust but creates many low-quality tetrahedra.");
        else if (volume_connection_method == tetra_viewer::VolumeConnectionMethod::fixed_surface_shell)
            ImGui::TextWrapped("One authoritative optimized boundary, a topology-matched graded connector, and unchanged hierarchy cells in the deep core.");
        else if (volume_connection_method == tetra_viewer::VolumeConnectionMethod::quality_stencils)
            ImGui::TextWrapped("Direct deterministic two-material stencils prioritize surface fairness and avoid artificial cell-centre vertices.");
        else if (volume_connection_method == tetra_viewer::VolumeConnectionMethod::adaptive_cleaving)
            ImGui::TextWrapped("Safe alpha warping removes near-endpoint cuts. It improves tetrahedron and triangle shape but may make the surface less uniform.");
        if (volume_connection_method == tetra_viewer::VolumeConnectionMethod::quality_stencils ||
            volume_connection_method == tetra_viewer::VolumeConnectionMethod::adaptive_cleaving) {
            ImGui::TextDisabled("Stencil construction");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##Stencil construction",
                                  tetra_viewer::stencil_construction_name(stencil_construction).data())) {
                for (std::size_t index=0;index<tetra_viewer::stencil_constructions.size();++index) {
                    const auto candidate=tetra_viewer::stencil_constructions[index];
                    const bool selected=candidate==stencil_construction;
                    ImGui::PushID(static_cast<int>(175+index));
                    if(ImGui::Selectable(tetra_viewer::stencil_construction_name(candidate).data(),selected))
                        stencil_construction=candidate;
                    if(selected)ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if(stencil_construction==tetra_viewer::StencilConstruction::selected){
                ImGui::TextDisabled("Stencil objective");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if(ImGui::BeginCombo("##Stencil objective",
                                     tetra_viewer::stencil_selection_objective_name(stencil_selection_objective).data())){
                    for(std::size_t index=0;index<tetra_viewer::stencil_selection_objectives.size();++index){
                        const auto candidate=tetra_viewer::stencil_selection_objectives[index];
                        const bool selected=candidate==stencil_selection_objective;
                        ImGui::PushID(static_cast<int>(185+index));
                        if(ImGui::Selectable(tetra_viewer::stencil_selection_objective_name(candidate).data(),selected))
                            stencil_selection_objective=candidate;
                        if(selected)ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::SeparatorText("Appearance");
        ImGui::TextDisabled("Shading model");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##Shading model", tetra_viewer::shading_model_name(shading_model).data())) {
            for (std::size_t model_index = 0; model_index < tetra_viewer::shading_models.size(); ++model_index) {
                const auto candidate = tetra_viewer::shading_models[model_index];
                const bool selected = candidate == shading_model;
                ImGui::PushID(static_cast<int>(200 + model_index));
                if (ImGui::Selectable(tetra_viewer::shading_model_name(candidate).data(), selected))
                    shading_model = candidate;
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (shading_model == tetra_viewer::ShadingModel::dihedral_angle ||
            shading_model == tetra_viewer::ShadingModel::normal_error)
            ImGui::TextWrapped("Fixed angle scale: blue <0.5 deg, cyan 1, green 2, yellow 5, orange 10, red 20, magenta 45, white 90.");
        const auto current_material_rule = tetra_viewer::material_rules[material_rule_index];
        ImGui::BeginDisabled(surface_method != tetra_viewer::SurfaceMethod::full_tetrahedra);
        ImGui::TextDisabled("Material rule");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##Material rule", tetra_viewer::material_rule_name(current_material_rule).data())) {
            for (std::size_t rule_index = 0; rule_index < tetra_viewer::material_rules.size(); ++rule_index) {
                const bool selected = rule_index == material_rule_index;
                ImGui::PushID(static_cast<int>(rule_index));
                if (ImGui::Selectable(tetra_viewer::material_rule_name(tetra_viewer::material_rules[rule_index]).data(), selected))
                    material_rule_index = rule_index;
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if(tetra_viewer::is_variational_material_rule(current_material_rule)){
            ImGui::TextWrapped("Global minimum cut selects complete hierarchy cells using field fidelity, face area, distance, and normal alignment.");
        }
        if (ImGui::BeginTable("display options", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); ImGui::Checkbox("Solid faces", &show_faces);
            ImGui::TableNextColumn(); ImGui::Checkbox("Surface edges", &show_surface_edges);
            ImGui::TableNextColumn(); ImGui::Checkbox("Hierarchy edges", &show_hierarchy_edges);
            ImGui::TableNextColumn(); ImGui::Checkbox("Depth colours", &depth_colours);
            ImGui::TableNextColumn(); ImGui::Checkbox("Volume edges", &show_volume_edges);
            ImGui::TableNextColumn(); ImGui::Checkbox("Solid volume", &show_volume_faces);
            ImGui::EndTable();
        }
        const bool supports_cutaway=tetra::has_capability(
            tetra::capabilities(adaptation_configuration.lod_update),
            tetra::AdaptationCapability::cutaway);
        ImGui::BeginDisabled(!supports_cutaway);
        ImGui::Checkbox("X cutaway", &x_cutaway);
        ImGui::EndDisabled();
        if (x_cutaway) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##X cut position", &x_cut_position, 0.0F, 1.0F, "x <= %.3f");
            ImGui::TextWrapped("Tetrahedra touching the right side are hidden; retained cells stay whole and do not protrude through the plane. Interior cells are blue and the conforming boundary layer is orange.");
        }
        ImGui::SeparatorText("Implicit shape and refinement");
        ImGui::TextDisabled("LOD update");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto lod_update_label=[](tetra::LodUpdateStrategy update){
            switch(update){
                case tetra::LodUpdateStrategy::transactional_active_cut:
                    return "Incremental active cut";
                case tetra::LodUpdateStrategy::saturated_clusters:
                    return "Saturated clusters";
                case tetra::LodUpdateStrategy::relevant_surface_hierarchy:
                    return "Relevant surface hierarchy";
                case tetra::LodUpdateStrategy::minimal_surface_hierarchy:
                    return "Minimal surface hierarchy";
                case tetra::LodUpdateStrategy::on_demand_render_traversal:
                    return "On-demand render traversal";
                case tetra::LodUpdateStrategy::full_rebuild_oracle:
                    return "Full rebuild oracle";
                default:return "Unavailable";
            }
        };
        const char* lod_update_name=lod_update_label(adaptation_configuration.lod_update);
        if(ImGui::BeginCombo("##LOD update",lod_update_name)){
            constexpr std::array updates{
                tetra::LodUpdateStrategy::transactional_active_cut,
                tetra::LodUpdateStrategy::saturated_clusters,
                tetra::LodUpdateStrategy::relevant_surface_hierarchy,
                tetra::LodUpdateStrategy::minimal_surface_hierarchy,
                tetra::LodUpdateStrategy::on_demand_render_traversal,
                tetra::LodUpdateStrategy::full_rebuild_oracle};
            for(const auto update:updates){
                const bool selected=update==adaptation_configuration.lod_update;
                const char* name=lod_update_label(update);
                auto candidate_configuration=adaptation_configuration;
                candidate_configuration.lod_update=update;
                const bool update_compatible=tetra::implemented(candidate_configuration)&&
                    (has_capability(tetra::capabilities(update),
                     tetra::AdaptationCapability::cutaway)||!x_cutaway);
                ImGui::BeginDisabled(!update_compatible);
                if(ImGui::Selectable(name,selected)&&!selected){
                    adaptation_configuration.lod_update=update;
                    lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
                ImGui::EndDisabled();
                if(selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Camera LOD policy");
        const auto camera_lod_name=tetra::strategy_key(
            adaptation_configuration.camera_lod_policy);
        const bool camera_lod_applied=
            adaptation_configuration.lod_update==
                tetra::LodUpdateStrategy::transactional_active_cut||
            adaptation_configuration.lod_update==
                tetra::LodUpdateStrategy::saturated_clusters;
        if(ImGui::BeginCombo("##Camera LOD policy",camera_lod_name.data())){
            constexpr std::array policies{
                tetra::CameraLodPolicy::exact_frustum,
                tetra::CameraLodPolicy::guarded,
                tetra::CameraLodPolicy::guarded_recent,
                tetra::CameraLodPolicy::guarded_predicted};
            for(const auto policy:policies){
                auto candidate=adaptation_configuration;
                candidate.camera_lod_policy=policy;
                const bool available=camera_lod_applied&&tetra::implemented(candidate);
                const bool selected=policy==adaptation_configuration.camera_lod_policy;
                ImGui::BeginDisabled(!available);
                if(ImGui::Selectable(tetra::strategy_key(policy).data(),selected)){
                    adaptation_configuration.camera_lod_policy=policy;
                    lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
                if(selected)ImGui::SetItemDefaultFocus();
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Camera LOD metric");
        const auto camera_lod_metric_name=tetra::strategy_key(
            adaptation_configuration.camera_lod_metric);
        if(ImGui::BeginCombo("##Camera LOD metric",camera_lod_metric_name.data())){
            constexpr std::array metrics{
                tetra::CameraLodMetric::projected_diameter,
                tetra::CameraLodMetric::geometric_error};
            for(const auto metric:metrics){
                auto candidate=adaptation_configuration;
                candidate.camera_lod_metric=metric;
                const bool available=camera_lod_applied&&tetra::implemented(candidate);
                const bool selected=metric==adaptation_configuration.camera_lod_metric;
                ImGui::BeginDisabled(!available);
                if(ImGui::Selectable(tetra::strategy_key(metric).data(),selected)){
                    adaptation_configuration.camera_lod_metric=metric;
                    lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
                if(selected)ImGui::SetItemDefaultFocus();
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if(ImGui::TreeNode("Camera LOD advanced")){
            const bool camera_controls_available=
                adaptation_configuration.lod_update==
                    tetra::LodUpdateStrategy::transactional_active_cut||
                adaptation_configuration.lod_update==
                    tetra::LodUpdateStrategy::saturated_clusters;
            ImGui::BeginDisabled(!camera_controls_available);
            float guard=static_cast<float>(adaptation_configuration.guard_frustum_scale);
            float near_radius=static_cast<float>(adaptation_configuration.near_radius);
            int retention=static_cast<int>(adaptation_configuration.recent_retention_epochs);
            float prediction=static_cast<float>(adaptation_configuration.prediction_factor);
            int complexity_target=static_cast<int>(
                adaptation_configuration.complexity_target_owners);
            bool changed=false;
            if(ImGui::Checkbox("Colour camera demand zones",
                               &show_camera_lod_zones))
                overlay_dirty=true;
            if(show_camera_lod_zones)
                ImGui::TextWrapped("Cell edges: visible white, near cyan, guard yellow, recent magenta, predicted orange, cold blue.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed|=ImGui::SliderFloat("##Guard expansion",&guard,1.0F,3.0F,
                                        "Guard %.2fx");
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed|=ImGui::SliderFloat("##Near radius",&near_radius,0.0F,4.0F,
                                        "Near %.2f");
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed|=ImGui::SliderInt("##Retention epochs",&retention,1,64,
                                      "Retain %d poses");
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed|=ImGui::SliderFloat("##Prediction factor",&prediction,0.0F,2.0F,
                                        "Predict %.2fx");
            ImGui::TextDisabled("Prediction mode");
            const auto prediction_name=tetra::strategy_key(
                adaptation_configuration.prediction_mode);
            if(ImGui::BeginCombo("##Prediction mode",prediction_name.data())){
                constexpr std::array modes{
                    tetra::CameraPredictionMode::none,
                    tetra::CameraPredictionMode::translation,
                    tetra::CameraPredictionMode::translation_rotation};
                for(const auto mode:modes){
                    const bool selected=mode==adaptation_configuration.prediction_mode;
                    if(ImGui::Selectable(tetra::strategy_key(mode).data(),selected)){
                        adaptation_configuration.prediction_mode=mode;
                        changed=true;
                    }
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed|=ImGui::SliderInt("##Complexity target",&complexity_target,
                                      0,200000,"Target %d owners");
            if(changed){
                adaptation_configuration.guard_frustum_scale=guard;
                adaptation_configuration.near_radius=near_radius;
                adaptation_configuration.recent_retention_epochs=
                    static_cast<std::uint32_t>(retention);
                adaptation_configuration.prediction_factor=prediction;
                adaptation_configuration.complexity_target_owners=
                    static_cast<std::uint32_t>(complexity_target);
                lod_reconcile_pending=true;
                has_adaptive_result=false;
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        const bool materialized_lod=
            adaptation_configuration.lod_update==
                tetra::LodUpdateStrategy::transactional_active_cut||
            adaptation_configuration.lod_update==
                tetra::LodUpdateStrategy::saturated_clusters;
        ImGui::TextDisabled("Candidate traversal");
        ImGui::BeginDisabled(!materialized_lod);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto traversal_label=[](tetra::CandidateTraversal traversal){
            switch(traversal){
                case tetra::CandidateTraversal::active_cut_scan:return "Active cut scan";
                case tetra::CandidateTraversal::hierarchy_bounds:return "Hierarchy bounds";
                case tetra::CandidateTraversal::spatial_runs:return "Spatial runs";
            }
            return "Unknown";
        };
        const char* traversal_name=traversal_label(
            adaptation_configuration.candidate_traversal);
        if(ImGui::BeginCombo("##Candidate traversal",traversal_name)){
            constexpr std::array traversals{
                tetra::CandidateTraversal::active_cut_scan,
                tetra::CandidateTraversal::hierarchy_bounds,
                tetra::CandidateTraversal::spatial_runs};
            for(const auto traversal:traversals){
                const bool selected=traversal==adaptation_configuration.candidate_traversal;
                const char* name=traversal_label(traversal);
                if(ImGui::Selectable(name,selected)&&!selected){
                    adaptation_configuration.candidate_traversal=traversal;
                    lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
                if(selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Closure execution");
        ImGui::BeginDisabled(!materialized_lod);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto closure_label=[](tetra::ClosureExecution closure){
            switch(closure){
                case tetra::ClosureExecution::sparse_frontier:return "Sparse frontier";
                case tetra::ClosureExecution::dense_level_sweep:return "Dense level sweep";
                case tetra::ClosureExecution::hybrid:return "Hybrid";
            }
            return "Unknown";
        };
        if(ImGui::BeginCombo("##Closure execution",
                            closure_label(adaptation_configuration.closure_execution))){
            constexpr std::array modes{
                tetra::ClosureExecution::sparse_frontier,
                tetra::ClosureExecution::dense_level_sweep,
                tetra::ClosureExecution::hybrid};
            for(const auto mode:modes){
                const bool selected=mode==adaptation_configuration.closure_execution;
                if(ImGui::Selectable(closure_label(mode),selected)&&!selected){
                    adaptation_configuration.closure_execution=mode;
                    lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
                if(selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if(adaptation_configuration.closure_execution==tetra::ClosureExecution::hybrid){
            float ratio=static_cast<float>(adaptation_configuration.hybrid_frontier_ratio);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::SliderFloat("##Hybrid closure ratio",&ratio,0.0F,1.0F,"Dense at %.2f")){
                adaptation_configuration.hybrid_frontier_ratio=ratio;
                lod_reconcile_pending=true;
                has_adaptive_result=false;
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Update scheduler");
        ImGui::BeginDisabled(!materialized_lod);
        const auto scheduler_label=[](tetra::UpdateScheduler scheduler){
            switch(scheduler){
                case tetra::UpdateScheduler::classify_and_stream:return "Classify and stream";
                case tetra::UpdateScheduler::persistent_split_merge_queues:
                    return "Persistent split / merge queues";
                case tetra::UpdateScheduler::hybrid_queued_blocks:return "Hybrid queued blocks";
            }
            return "Unknown";
        };
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##Update scheduler",
                            scheduler_label(adaptation_configuration.update_scheduler))){
            constexpr std::array schedulers{
                tetra::UpdateScheduler::classify_and_stream,
                tetra::UpdateScheduler::persistent_split_merge_queues,
                tetra::UpdateScheduler::hybrid_queued_blocks};
            for(const auto scheduler:schedulers){
                const bool selected=scheduler==adaptation_configuration.update_scheduler;
                if(ImGui::Selectable(scheduler_label(scheduler),selected)&&!selected){
                    adaptation_configuration.update_scheduler=scheduler;
                    adaptation_planning_cache.clear();lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Layer storage (rebuild experiment)");
        const auto storage_label=[](tetra::LayerStorage storage){
            switch(storage){
                case tetra::LayerStorage::flat_packed:return "Flat packed";
                case tetra::LayerStorage::mutable_macro_blocks:return "Mutable macro blocks";
                case tetra::LayerStorage::occupancy_bit_macro_blocks:
                    return "Occupancy-bit macro blocks";
                case tetra::LayerStorage::address_runs:return "Address runs";
            }
            return "Unknown";
        };
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##Layer storage",
                            storage_label(adaptation_configuration.layer_storage))){
            constexpr std::array storages{
                tetra::LayerStorage::flat_packed,
                tetra::LayerStorage::mutable_macro_blocks,
                tetra::LayerStorage::occupancy_bit_macro_blocks,
                tetra::LayerStorage::address_runs};
            for(const auto storage:storages){
                const bool selected=storage==adaptation_configuration.layer_storage;
                if(ImGui::Selectable(storage_label(storage),selected)&&!selected){
                    adaptation_configuration.layer_storage=storage;
                    interactive_storage_experiment=tetra::build_layer_storage_experiment(
                        mesh,sphere,storage,adaptation_configuration.kernel_order);
                    adaptation_planning_cache.clear();lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Kernel order");
        const auto kernel_label=[](tetra::KernelOrder order){
            switch(order){
                case tetra::KernelOrder::address_order:return "Address order";
                case tetra::KernelOrder::orientation_buckets:return "Orientation buckets";
                case tetra::KernelOrder::fused_macro_blocks:return "Fused macro blocks";
            }
            return "Unknown";
        };
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##Kernel order",
                            kernel_label(adaptation_configuration.kernel_order))){
            constexpr std::array orders{
                tetra::KernelOrder::address_order,
                tetra::KernelOrder::orientation_buckets,
                tetra::KernelOrder::fused_macro_blocks};
            for(const auto order:orders){
                const bool selected=order==adaptation_configuration.kernel_order;
                if(ImGui::Selectable(kernel_label(order),selected)&&!selected){
                    adaptation_configuration.kernel_order=order;
                    interactive_storage_experiment=tetra::build_layer_storage_experiment(
                        mesh,sphere,adaptation_configuration.layer_storage,order);
                    adaptation_planning_cache.clear();lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Adjacency representation");
        const auto adjacency_label=[](tetra::AdjacencyRepresentation adjacency){
            switch(adjacency){
                case tetra::AdjacencyRepresentation::path_arithmetic:return "Path arithmetic";
                case tetra::AdjacencyRepresentation::packed_half_facets:return "Packed half facets";
                case tetra::AdjacencyRepresentation::logical_face_table:return "Logical face table";
                case tetra::AdjacencyRepresentation::reconstruction_oracle:
                    return "Reconstruction oracle";
            }
            return "Unknown";
        };
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##Adjacency",
                            adjacency_label(adaptation_configuration.adjacency))){
            constexpr std::array representations{
                tetra::AdjacencyRepresentation::path_arithmetic,
                tetra::AdjacencyRepresentation::packed_half_facets,
                tetra::AdjacencyRepresentation::logical_face_table,
                tetra::AdjacencyRepresentation::reconstruction_oracle};
            for(const auto adjacency:representations){
                const bool selected=adjacency==adaptation_configuration.adjacency;
                if(ImGui::Selectable(adjacency_label(adjacency),selected)&&!selected){
                    adaptation_configuration.adjacency=adjacency;
                    interactive_adjacency_experiment=
                        tetra::build_adjacency_experiment(mesh,adjacency);
                    adaptation_planning_cache.clear();lod_reconcile_pending=true;
                    has_adaptive_result=false;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Transition closure");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* transition_name=adaptation_configuration.transition_strategy==
            tetra::BccTransitionStrategy::crystalline_restricted
            ?"Crystalline restricted":"Complete minimal";
        if(ImGui::BeginCombo("##Transition closure",transition_name)){
            constexpr std::array strategies{
                tetra::BccTransitionStrategy::crystalline_restricted,
                tetra::BccTransitionStrategy::complete_minimal};
            for(const auto strategy:strategies){
                const bool selected=strategy==adaptation_configuration.transition_strategy;
                const char* name=strategy==tetra::BccTransitionStrategy::crystalline_restricted
                    ?"Crystalline restricted":"Complete minimal";
                if(ImGui::Selectable(name,selected)&&!selected){
                    if(mesh.set_transition_strategy(strategy)){
                        adaptation_configuration.transition_strategy=strategy;
                        adaptation_planning_cache.clear();
                        lod_reconcile_pending=true;
                        has_adaptive_result=false;
                    }
                }
                if(selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Shape");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##Implicit shape",tetra::implicit_shape_name(sphere.kind).data())){
            for(const auto kind:tetra::implicit_shape_kinds){
                const bool selected=kind==sphere.kind;
                if(ImGui::Selectable(tetra::implicit_shape_name(kind).data(),selected)&&!selected){
                    sphere.kind=kind;
                    sphere.secondary=tetra::implicit_shape_default_secondary(kind);
                    ++sphere_revision;
                    has_adaptive_result=false;
                    lod_reconcile_pending=true;
                }
                if(selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Shape centre");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat3("##Sphere centre", sphere_centre, 0.05f, 0.95f)) {
            sphere.centre = {sphere_centre[0], sphere_centre[1], sphere_centre[2]};
            ++sphere_revision;
            has_adaptive_result = false;
            lod_reconcile_pending=true;
        }
        if(sphere.kind!=tetra::ImplicitShapeKind::perlin_terrain&&
           sphere.kind!=tetra::ImplicitShapeKind::gyroid){
            ImGui::TextDisabled("Shape scale");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##Shape scale", &sphere_radius, 0.05f, 0.48f)) {
                sphere.radius = sphere_radius;
                ++sphere_revision;
                has_adaptive_result = false;
                lod_reconcile_pending=true;
            }
        }
        if(sphere.kind==tetra::ImplicitShapeKind::merging_spheres||
           sphere.kind==tetra::ImplicitShapeKind::torus||
           sphere.kind==tetra::ImplicitShapeKind::rounded_cube||
           sphere.kind==tetra::ImplicitShapeKind::perlin_terrain||
           sphere.kind==tetra::ImplicitShapeKind::gyroid){
            const char* parameter_label="Shape parameter";
            switch(sphere.kind){
                case tetra::ImplicitShapeKind::merging_spheres:parameter_label="Sphere separation";break;
                case tetra::ImplicitShapeKind::torus:parameter_label="Tube radius";break;
                case tetra::ImplicitShapeKind::rounded_cube:parameter_label="Corner radius";break;
                case tetra::ImplicitShapeKind::perlin_terrain:parameter_label="Terrain amplitude";break;
                case tetra::ImplicitShapeKind::gyroid:parameter_label="Surface threshold";break;
                default:break;
            }
            ImGui::TextDisabled("%s",parameter_label);
            float secondary=static_cast<float>(sphere.secondary);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::SliderFloat("##Shape secondary",&secondary,0.01F,0.30F,"%.3f")){
                sphere.secondary=secondary;++sphere_revision;
                has_adaptive_result=false;lod_reconcile_pending=true;
            }
        }
        if(sphere.kind==tetra::ImplicitShapeKind::perlin_terrain||
           sphere.kind==tetra::ImplicitShapeKind::gyroid){
            ImGui::TextDisabled("Frequency");
            float frequency=static_cast<float>(sphere.frequency);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::SliderFloat("##Shape frequency",&frequency,1.0F,8.0F,"%.2f")){
                sphere.frequency=frequency;++sphere_revision;
                has_adaptive_result=false;lod_reconcile_pending=true;
            }
        }
        ImGui::SeparatorText("LOD camera");
        ImGui::TextWrapped("Click the camera, then use Q/W/E for select, move, or rotate. Releasing a gizmo incrementally reconciles the active cut with the new view.");
        ImGui::TextDisabled("Origin: %.2f, %.2f, %.2f   Direction: %.2f, %.2f, %.2f",
                            camera.position.x,camera.position.y,camera.position.z,
                            camera.forward.x,camera.forward.y,camera.forward.z);
        const auto feedback_handle=camera_manipulator.dragging()?
            camera_manipulator.active:(camera_manipulator.preferred!=
                tetra_viewer::CameraHandle::none?camera_manipulator.preferred:
                camera_manipulator.hovered);
        ImGui::TextDisabled("Space: %s   Handle: %s%s",
            camera_manipulator.space==tetra_viewer::ManipulatorSpace::world?"World":"Local",
            tetra_viewer::camera_handle_name(feedback_handle).data(),
            camera_manipulator.dragging()?" (active)":"");
        if(camera_manipulator.dragging()){
            if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::rotate)
                ImGui::Text("Rotation: %.2f deg",
                    camera_manipulator.displayed_delta()*180.0/std::acos(-1.0));
            else ImGui::Text("Movement: %.4f",camera_manipulator.displayed_delta());
        }
        if(ImGui::BeginTable("camera gizmo modes",3,ImGuiTableFlags_SizingStretchSame)){
            const auto mode_button=[&](const char* label,tetra_viewer::CameraGizmoMode mode){
                ImGui::TableNextColumn();
                const bool selected=mode==camera_gizmo_mode;
                if(selected)ImGui::PushStyleColor(
                    ImGuiCol_Button,ImVec4(0.22f,0.48f,0.72f,1.0f));
                if(ImGui::Button(label,ImVec2(-FLT_MIN,0.0f))){
                    camera_gizmo_mode=mode;
                    upload_dirty=true;
                }
                if(selected)ImGui::PopStyleColor();
            };
            mode_button("Select (Q)",tetra_viewer::CameraGizmoMode::select);
            mode_button("Move (W)",tetra_viewer::CameraGizmoMode::translate);
            mode_button("Rotate (E)",tetra_viewer::CameraGizmoMode::rotate);
            ImGui::EndTable();
        }
        if(ImGui::BeginTable("camera gizmo space",2,ImGuiTableFlags_SizingStretchSame)){
            const auto space_button=[&](const char* label,tetra_viewer::ManipulatorSpace space){
                ImGui::TableNextColumn();
                const bool selected=camera_manipulator.space==space;
                if(selected)ImGui::PushStyleColor(
                    ImGuiCol_Button,ImVec4(0.22f,0.48f,0.72f,1.0f));
                if(ImGui::Button(label,ImVec2(-FLT_MIN,0.0f))){
                    camera_manipulator.space=space;upload_dirty=true;
                }
                if(selected)ImGui::PopStyleColor();
            };
            space_button("World",tetra_viewer::ManipulatorSpace::world);
            space_button("Local",tetra_viewer::ManipulatorSpace::local);
            ImGui::EndTable();
        }
        ImGui::Checkbox("Snap manipulator",&camera_manipulator.snap.enabled);
        if(camera_manipulator.snap.enabled){
            int snap_mode=camera_manipulator.snap.mode==
                tetra_viewer::ManipulatorSnapSettings::Mode::relative?0:1;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::Combo("##Snap mode",&snap_mode,"Relative\0World absolute\0"))
                camera_manipulator.snap.mode=snap_mode==0?
                    tetra_viewer::ManipulatorSnapSettings::Mode::relative:
                    tetra_viewer::ManipulatorSnapSettings::Mode::absolute;
            float translation_step=static_cast<float>(camera_manipulator.snap.translation_step);
            float rotation_degrees=static_cast<float>(
                camera_manipulator.snap.rotation_step_radians*180.0/std::acos(-1.0));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::DragFloat("##Translation snap",&translation_step,0.005F,0.001F,1.0F,
                                "Move %.3f"))
                camera_manipulator.snap.translation_step=translation_step;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::DragFloat("##Rotation snap",&rotation_degrees,0.5F,0.1F,90.0F,
                                "Rotate %.1f deg"))
                camera_manipulator.snap.rotation_step_radians=
                    rotation_degrees*std::acos(-1.0)/180.0;
        }
        if(ImGui::Button("Undo camera",ImVec2(ImGui::GetContentRegionAvail().x*0.5F-2.0F,0.0F))){
            if(camera_manipulator.undo(lod_camera_pose)){
                lod_camera_pose.apply(camera);lod_reconcile_pending=true;upload_dirty=true;
            }
        }
        ImGui::SameLine();
        if(ImGui::Button("Redo camera",ImVec2(-FLT_MIN,0.0F))){
            if(camera_manipulator.redo(lod_camera_pose)){
                lod_camera_pose.apply(camera);lod_reconcile_pending=true;upload_dirty=true;
            }
        }
        if(ImGui::Button("Place LOD camera at view",ImVec2(-FLT_MIN,0.0f))){
            lod_camera_pose.position=view_camera_position;
            lod_camera_pose.forward=orbit_camera.forward();
            lod_camera_pose.up=orbit_camera.up();
            lod_camera_pose.apply(camera);
            lod_camera_selected=true;
            has_adaptive_result=false;
            lod_reconcile_pending=true;
            upload_dirty=true;
        }
        ImGui::SeparatorText("Editor view");
        ImGui::TextWrapped("Drag empty space: orbit   Shift-drag: pan   Option works too   Scroll: dolly");
        if(ImGui::Button("Frame shape",ImVec2(-FLT_MIN,0.0f))){
            orbit_camera.target=sphere.centre;
            orbit_camera.distance=2.5;
            orbit_camera.yaw=0.0;
            orbit_camera.pitch=0.0;
            update_orbit_camera();
            has_adaptive_result=false;
        }
        ImGui::TextDisabled("Pixel threshold");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##Pixel threshold", &pixel_threshold, 4.0f, 240.0f, "%.0f px")){
            has_adaptive_result = false;
            lod_reconcile_pending=true;
        }
        ImGui::TextDisabled("Maximum depth");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##Maximum depth", &maximum_depth, 1, 32)) {
            has_adaptive_result = false;
            lod_reconcile_pending=true;
        }
        update_orbit_camera();
        const double viewport_height = static_cast<double>(std::max(1.0F, ImGui::GetIO().DisplaySize.y));
        const double viewport_aspect=
            static_cast<double>(std::max(1.0F,ImGui::GetIO().DisplaySize.x))/viewport_height;
        if(camera.viewport_height_pixels!=viewport_height||
           camera.aspect_ratio!=viewport_aspect){
            lod_reconcile_pending=true;
            overlay_dirty=true;
        }
        camera.viewport_height_pixels = viewport_height;
        camera.aspect_ratio=viewport_aspect;
        if (ImGui::BeginTable("mesh actions", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button("Reset", ImVec2(-FLT_MIN, 0.0f))) {
            if(mesh_update_in_flight){
                mesh_update_worker.cancel();
                mesh_update_in_flight=false;
                submitted_mesh_update.reset();
                submitted_mesh_request_id=0U;
            }
            mesh = tetra::TetMesh::make_unit_cube(tetra::subdivision_methods[subdivision_method_index]);
            if(mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green)
                static_cast<void>(mesh.set_transition_strategy(
                    adaptation_configuration.transition_strategy));
            implicit_value_cache.clear();
            refined = false;
            has_adaptive_result = false;
            mesh_valid = true;
            mesh_validation_current = true;
            last_validation_milliseconds = -1.0;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Refine once", ImVec2(-FLT_MIN, 0.0f))) {
            const auto parameters=mesh_update_parameters();
            submitted_mesh_request_id=mesh_update_worker.submit(
                mesh,parameters,tetra_viewer::MeshUpdateOperation::refine_all_once);
            submitted_mesh_update=parameters;
            submitted_mesh_operation=
                tetra_viewer::MeshUpdateOperation::refine_all_once;
            submitted_mesh_revision=mesh.revision();
            mesh_update_in_flight=true;
            lod_reconcile_pending=false;
            has_adaptive_result=false;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Refine to target", ImVec2(-FLT_MIN, 0.0f))) {
            const bool background_supported=
                mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green&&
                (adaptation_configuration.lod_update==
                     tetra::LodUpdateStrategy::transactional_active_cut||
                 adaptation_configuration.lod_update==
                     tetra::LodUpdateStrategy::saturated_clusters);
            if(background_supported){
                has_adaptive_result=false;
                lod_reconcile_pending=true;
            }else{
                const auto start = std::chrono::steady_clock::now();
                last_adaptive_result = reconcile_to_current_surface();
                last_refine_milliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                has_adaptive_result = true;
                refined = true;
                mesh_validation_current = false;
                lod_reconcile_pending=!reconcile_complete;
            }
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Validate", ImVec2(-FLT_MIN, 0.0f))) {
            const auto validation_start = std::chrono::steady_clock::now();
            mesh_valid = validate_mesh();
            last_validation_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - validation_start).count();
            mesh_validation_current = true;
        }
        ImGui::EndTable();
        }
        if (has_adaptive_result) {
            if (last_adaptive_result.iterations != 0) {
                ImGui::Text("Target refinement: %zu passes, %zu marks",
                            last_adaptive_result.iterations, last_adaptive_result.refined_leaves);
            } else if (last_adaptive_result.reached_depth_limit) {
                const unsigned int increment = tetra::subdivision_depth_increment(
                    tetra::subdivision_methods[subdivision_method_index]);
                const unsigned int next_depth =
                    (static_cast<unsigned int>(maximum_depth) / increment + 1U) * increment;
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                                   "No change: depth limit reached (next usable depth %u)", next_depth);
            } else {
                ImGui::TextDisabled("No change: the pixel target is already satisfied");
            }
        }
        ImGui::TextDisabled("%s", refined ? "Refined mesh" : "Seed mesh");
        if(mesh_update_in_flight)
            ImGui::TextColored(ImVec4(0.42f,0.78f,1.0f,1.0f),
                               "Updating mesh in background...");
        if(!retained_upload_check&&scene_preparation_worker.busy())
            ImGui::TextColored(ImVec4(0.42f,0.78f,1.0f,1.0f),
                               "Preparing geometry in background...");
        const bool statistics_open=ImGui::CollapsingHeader("Statistics");
        const bool diagnostic_shading=
            shading_model==tetra_viewer::ShadingModel::dihedral_angle||
            shading_model==tetra_viewer::ShadingModel::normal_error;
        const tetra_viewer::ScenePreparationOptions preparation{
            .surface_diagnostics=statistics_open||diagnostic_shading,
            .summary_statistics=statistics_open};
        const tetra_viewer::ScenePreparationParameters scene_parameters{
            .surface=sphere,.surface_revision=sphere_revision,
            .surface_method=surface_method,
            .material_rule=tetra_viewer::material_rules[material_rule_index],
            .show_faces=show_faces,
            .show_hierarchy_edges=show_hierarchy_edges,
            .show_surface_edges=show_surface_edges,
            .depth_colours=depth_colours,
            .show_volume_edges=x_cutaway&&show_volume_edges,
            .show_volume_faces=x_cutaway&&show_volume_faces,
            .x_cut_position=x_cut_position,
            .volume_connection_method=volume_connection_method,
            .stencil_construction=stencil_construction,
            .stencil_selection_objective=stencil_selection_objective,
            .preparation=preparation,
            .surface_override_revision=fixed_field_surface_cut_revision};
        if(world_mode){
            retained_surface_upload_ready=world_runtime&&
                world_runtime->retained_surface()!=nullptr;
        }else if(retained_upload_check){
            const auto preparation_start=std::chrono::steady_clock::now();
            if(scene_cache.update_scene(
                   mesh,sphere,sphere_revision,surface_method,
                   tetra_viewer::material_rules[material_rule_index],show_faces,
                   show_hierarchy_edges,show_surface_edges,depth_colours,
                   x_cutaway&&show_volume_edges,x_cutaway&&show_volume_faces,
                   x_cut_position,volume_connection_method,stencil_construction,
                   stencil_selection_objective,preparation,
                   fixed_field_surface_triangles,fixed_field_surface_cut_revision,
                   geometry_executor.get())){
              last_scene_preparation_milliseconds=
                  std::chrono::duration<double,std::milli>(
                      std::chrono::steady_clock::now()-preparation_start).count();
              const auto& patch_metrics=scene_cache.surface_patch_metrics();
              retained_surface_upload_ready=patch_metrics.active&&
                  scene_cache.scene().triangle_vertices.size()==
                      patch_metrics.output_triangles*3U;
              if(retained_surface_upload_ready){
                surface_draw_chunks.pack(scene_cache.surface_patch_records(),
                                         scene_cache.surface_patch_arena(),
                                         geometry_executor.get());
                surface_host_staging.stage(
                    surface_draw_chunks,scene_cache.scene().triangle_vertices,
                    geometry_executor.get());
              }
              upload_dirty=true;
            }
        }else{
          retained_surface_upload_ready=false;
          const bool interactive_scene_update=camera_manipulator.dragging();
          if(auto completed=scene_preparation_worker.take_completed()){
            if(tetra_viewer::compatible_scene_preparation_publication(
                   *completed,submitted_scene_request_id,mesh.revision(),
                   scene_parameters,interactive_scene_update)){
              background_prepared_scene=std::move(completed->scene);
              prepared_scene_mesh_revision=completed->mesh_revision;
              last_scene_preparation_milliseconds=completed->duration_milliseconds;
              upload_dirty=true;
            }
          }
          const bool request_changed=!submitted_scene_preparation||
              submitted_scene_mesh_revision!=mesh.revision()||
              !tetra_viewer::same_scene_preparation_parameters(
                  *submitted_scene_preparation,scene_parameters);
          if(tetra_viewer::should_submit_scene_preparation(
                 request_changed,scene_preparation_worker.busy(),
                 interactive_scene_update)){
            submitted_scene_request_id=scene_preparation_worker.submit(
                mesh,scene_parameters,fixed_field_surface_triangles);
            submitted_scene_preparation=scene_parameters;
            submitted_scene_mesh_revision=mesh.revision();
          }
        }
        const auto& prepared_scene=retained_upload_check
            ?scene_cache.scene():background_prepared_scene;
        if(statistics_open&&prepared_scene_mesh_revision==mesh.revision())
          background_projection_statistics=tetra_viewer::prepare_projection_statistics(
              mesh,prepared_scene,camera,pixel_threshold);
        const auto& projection_statistics=retained_upload_check
            ?scene_cache.projection():background_projection_statistics;
        if (statistics_open) {
        ImGui::Text("Conforming cells: %zu", mesh.conforming_volume().size());
        const auto temporal_entries=std::accumulate(
            adaptation_planning_cache.camera_temporal_layers.begin(),
            adaptation_planning_cache.camera_temporal_layers.end(),std::size_t{},
            [](std::size_t count,const tetra::CameraTemporalLayer& layer){
                return count+layer.addresses.size();
            });
        ImGui::Text("Camera LOD: %s / %s",
                    tetra::strategy_key(adaptation_configuration.camera_lod_policy).data(),
                    tetra::strategy_key(adaptation_configuration.camera_lod_metric).data());
        ImGui::Text("Camera working set: %zu owners  %zu retained records",
                    adaptation_planning_cache.last_camera_active_owner_count,
                    temporal_entries);
        ImGui::Text("Soft quality: %.2fx  Epoch: %llu",
                    adaptation_planning_cache.camera_soft_quality_multiplier,
                    static_cast<unsigned long long>(
                        adaptation_planning_cache.camera_demand_epoch));
        ImGui::Text("Demand cold/recent/predicted: %zu / %zu / %zu",
                    adaptation_planning_cache.last_camera_demand_evaluations[
                        static_cast<std::size_t>(tetra::CameraLodZone::cold)],
                    adaptation_planning_cache.last_camera_demand_evaluations[
                        static_cast<std::size_t>(tetra::CameraLodZone::recent)],
                    adaptation_planning_cache.last_camera_demand_evaluations[
                        static_cast<std::size_t>(tetra::CameraLodZone::predicted)]);
        ImGui::Text("Demand guard/near/visible: %zu / %zu / %zu",
                    adaptation_planning_cache.last_camera_demand_evaluations[
                        static_cast<std::size_t>(tetra::CameraLodZone::guard)],
                    adaptation_planning_cache.last_camera_demand_evaluations[
                        static_cast<std::size_t>(tetra::CameraLodZone::near)],
                    adaptation_planning_cache.last_camera_demand_evaluations[
                        static_cast<std::size_t>(tetra::CameraLodZone::visible)]);
        std::vector<double> visible_projection_errors;
        std::size_t ready_visible{};
        for(const auto owner:mesh.logical_red_owners()){
            const auto demand=tetra::camera_lod_demand(
                mesh,owner,camera,adaptation_configuration);
            if(demand.zone!=tetra::CameraLodZone::visible)continue;
            visible_projection_errors.push_back(demand.projected_diameter_pixels);
            ready_visible+=demand.projected_diameter_pixels<=
                pixel_threshold*adaptation_configuration.split_hysteresis;
        }
        std::sort(visible_projection_errors.begin(),visible_projection_errors.end());
        const auto visible_percentile=[&](double fraction){
            if(visible_projection_errors.empty())return 0.0;
            const auto index=static_cast<std::size_t>(fraction*static_cast<double>(
                visible_projection_errors.size()-1U));
            return visible_projection_errors[index];
        };
        const double readiness=visible_projection_errors.empty()?1.0:
            static_cast<double>(ready_visible)/visible_projection_errors.size();
        ImGui::Text("Visible projected error: max %.1f px  p95 %.1f px",
                    visible_projection_errors.empty()?0.0:visible_projection_errors.back(),
                    visible_percentile(0.95));
        ImGui::Text("Turn readiness proxy: %.1f%%  Convergence: %s",
                    readiness*100.0,
                    mesh_update_in_flight||lod_reconcile_pending?"updating":"stable");
        std::size_t retained_bytes{};
        for(const auto& layer:adaptation_planning_cache.camera_temporal_layers)
            retained_bytes+=layer.addresses.capacity()*sizeof(tetra::TetId)+
                layer.last_demand_epochs.capacity()*sizeof(std::uint64_t);
        const auto active_owners=mesh.logical_red_owners().size();
        const bool above_complexity_limit=
            adaptation_configuration.complexity_target_owners>0U&&
            active_owners>static_cast<std::size_t>(
                adaptation_configuration.complexity_target_owners*
                (1.0+adaptation_configuration.complexity_band));
        ImGui::Text("Temporal demand storage: %.1f KiB",retained_bytes/1024.0);
        if(above_complexity_limit)
            ImGui::TextColored(ImVec4(1.0F,0.55F,0.22F,1.0F),
                               "Soft complexity target exceeded");
        ImGui::Text("Total volume: %.6f", prepared_scene.total_volume);
        ImGui::Text("Validation: %s", mesh_validation_current ? (mesh_valid ? "PASS" : "FAIL") : "NOT RUN");
        ImGui::Text("Selected: %zu   Inside: %zu", prepared_scene.selected_count, prepared_scene.inside_count);
        ImGui::Text("Outside: %zu", prepared_scene.outside_count);
        if(prepared_scene.whole_cell_hash!=0){
            ImGui::Text("Whole-cell boundary: %zu faces  %.3f volume",
                        prepared_scene.whole_cell_boundary_faces,
                        prepared_scene.whole_cell_selected_volume);
            ImGui::Text("Cut solve: %.2f ms  Non-manifold edges: %zu",
                        prepared_scene.whole_cell_solve_milliseconds,
                        prepared_scene.whole_cell_nonmanifold_edges);
        }
        if (prepared_scene.surface_layer_tetrahedra != 0)
            ImGui::Text("Surface-layer tetrahedra: %zu", prepared_scene.surface_layer_tetrahedra);
        if (prepared_scene.volume_internal_edges != 0 || prepared_scene.volume_boundary_edges != 0)
            ImGui::Text("Volume edges: %zu internal  %zu boundary",
                        prepared_scene.volume_internal_edges, prepared_scene.volume_boundary_edges);
        if (prepared_scene.visible_volume_face_triangles != 0)
            ImGui::Text("Visible volume faces: %zu triangles", prepared_scene.visible_volume_face_triangles);
        if (prepared_scene.connected_surface_edges != 0)
            ImGui::Text("Connected surface edges: %zu", prepared_scene.connected_surface_edges);
        if (!prepared_scene.connected_volume_tetrahedra.empty())
            ImGui::Text("Connected volume: %zu vertices  %zu tetrahedra",
                        prepared_scene.connected_volume_vertices.size(),
                        prepared_scene.connected_volume_tetrahedra.size());
        if (prepared_scene.marching_tetrahedra_triangles != 0)
            ImGui::Text("Marching-tetrahedra triangles: %zu", prepared_scene.marching_tetrahedra_triangles);
        if (prepared_scene.cleaved_tetrahedra != 0)
            ImGui::Text("Cleaved boundary tetrahedra: %zu", prepared_scene.cleaved_tetrahedra);
        if (prepared_scene.dual_contour_triangles != 0)
            ImGui::Text("Dual-contour triangles: %zu", prepared_scene.dual_contour_triangles);
        if (prepared_scene.optimized_surface_vertices != 0)
            ImGui::Text("Optimized vertices: %zu  Rejected moves: %zu",
                        prepared_scene.optimized_surface_vertices,
                        prepared_scene.rejected_surface_moves+
                            prepared_scene.rejected_volume_boundary_moves);
        if (!prepared_scene.connected_volume_tetrahedra.empty()) {
            ImGui::Text("Connected moves: %zu accepted  %zu rejected",
                        prepared_scene.optimized_volume_boundary_vertices,
                        prepared_scene.rejected_volume_boundary_moves);
            ImGui::Text("Minimum tet quality: %.5f -> %.5f",
                        prepared_scene.minimum_connected_tet_quality_before,
                        prepared_scene.minimum_connected_tet_quality_after);
        }
        if (!prepared_scene.triangle_vertices.empty()) {
            ImGui::Text("Dihedral: mean %.2f  p95 %.2f  p99 %.2f  max %.2f deg",
                        prepared_scene.mean_dihedral_degrees,
                        prepared_scene.percentile95_dihedral_degrees,
                        prepared_scene.percentile99_dihedral_degrees,
                        prepared_scene.maximum_dihedral_degrees);
            ImGui::Text("Normal error: mean %.2f  p95 %.2f  p99 %.2f  max %.2f deg",
                        prepared_scene.mean_normal_error_degrees,
                        prepared_scene.percentile95_normal_error_degrees,
                        prepared_scene.percentile99_normal_error_degrees,
                        prepared_scene.maximum_normal_error_degrees);
            ImGui::Text("Triangle: min angle %.2f deg  max edge ratio %.2f",
                        prepared_scene.minimum_surface_triangle_angle_degrees,
                        prepared_scene.maximum_surface_triangle_edge_ratio);
        }
        ImGui::Text("Pending: %zu   Accepted: %zu",
                    projection_statistics.pending_count, projection_statistics.accepted_count);
        std::string depth_summary = "Leaf depths:";
        for (std::size_t depth = 0; depth < prepared_scene.depth_counts.size(); ++depth)
            if (prepared_scene.depth_counts[depth] != 0)
                depth_summary += "  L" + std::to_string(depth) + " " + std::to_string(prepared_scene.depth_counts[depth]);
        ImGui::TextWrapped("%s", depth_summary.c_str());
        if (last_refine_milliseconds >= 0.0) ImGui::Text("Mesh edit: %.1f ms", last_refine_milliseconds);
        if (last_validation_milliseconds >= 0.0) ImGui::Text("Validation: %.1f ms", last_validation_milliseconds);
        if (last_scene_preparation_milliseconds >= 0.0) ImGui::Text("Scene preparation: %.1f ms", last_scene_preparation_milliseconds);
        const auto executor_metrics=geometry_executor->metrics();
        ImGui::Text("Geometry workers: %zu  active peak: %zu",
                    geometry_executor->worker_count(),
                    executor_metrics.maximum_active_workers);
        ImGui::Text("Queue peak: %zu  helped: %zu  nested: %zu",
                    executor_metrics.maximum_queued_tasks,
                    executor_metrics.stolen_or_helped_tasks,
                    executor_metrics.nested_executor_entries);
        if(prepared_scene.parallel_classification_tasks!=0U)
            ImGui::Text("Parallel classification: %zu tasks  %.2f ms",
                        prepared_scene.parallel_classification_tasks,
                        prepared_scene.parallel_classification_milliseconds);
        if(prepared_scene.parallel_render_attribute_tasks!=0U)
            ImGui::Text("Parallel render attributes: %zu tasks  %.2f ms",
                        prepared_scene.parallel_render_attribute_tasks,
                        prepared_scene.parallel_render_attribute_milliseconds);
        }
        bool controls_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::End();
        if(world_mode){
            ImGui::SetNextWindowPos(ImVec2(14.0F,14.0F),ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.78F);
            ImGui::Begin("World status",nullptr,
                ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
                ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextUnformatted("WASD move  Shift sprint  Space jump");
            ImGui::TextUnformatted("Ctrl super speed  Ctrl+Shift 10x super speed");
            ImGui::TextUnformatted("Mouse look  Esc releases pointer  Click captures");
            ImGui::Separator();
            const float frame_rate=ImGui::GetIO().Framerate;
            ImGui::Text("Frame %.2f ms   %.1f FPS",
                        frame_rate>0.0F?1000.0F/frame_rate:0.0F,frame_rate);
            if(!world_runtime){
                ImGui::TextUnformatted("Terrain loading...");
            }else{
                const auto status=world_runtime->diagnostics();
                ImGui::Text("Terrain %s",status.busy?"updating...":"ready");
                ImGui::Text("Cells %zu   tetrahedra %zu",status.logical_cells,
                            status.active_tetrahedra);
                ImGui::Text("Mesh revision %llu   %.2f ms",
                            static_cast<unsigned long long>(status.mesh_revision),
                            status.last_update_milliseconds);
                ImGui::Text("World %.0f units   revision %llu",
                            status.world_extent,
                            static_cast<unsigned long long>(status.world_revision));
                ImGui::Text("Blocks %zu   surface blocks %zu",
                            status.hierarchy_blocks,status.surface_blocks);
            }
            const auto& world_camera=world_controller.state();
            ImGui::Text("Position %.3f  %.3f  %.3f",
                        world_camera.feet.x,world_camera.feet.y,
                        world_camera.feet.z);
            ImGui::Text("Rotation yaw %.1f deg   pitch %.1f deg",
                        std::remainder(world_camera.yaw*180.0/
                                           std::numbers::pi,360.0),
                        world_camera.pitch*180.0/std::numbers::pi);
            ImGui::Separator();
            CheckboxWithHotkey("Pause simulation","P",ImGuiKey_P,&world_paused);
            ImGui::SameLine();
            if(ImGui::Button("Single step"))world_single_step=true;
            CheckboxWithHotkey("Free fly","F",ImGuiKey_F,&world_free_fly);
            CheckboxWithHotkey("Lock terrain LOD camera","G",ImGuiKey_G,
                               &world_lock_lod_camera);
            if(!world_free_fly)
                ImGui::TextDisabled("Lock applies while free flying");
            CheckboxWithHotkey("Triangle wireframe","T",ImGuiKey_T,
                               &show_surface_edges);
            CheckboxWithHotkey("Smooth terrain normals","M",ImGuiKey_M,
                               &world_smooth_normals);
            if(!g_SceneRenderer.supports_terrain_msaa())ImGui::BeginDisabled();
            if(ImGui::Checkbox("4x terrain MSAA",&world_terrain_msaa)){
                if(vkDeviceWaitIdle(g_Device)!=VK_SUCCESS)
                    throw std::runtime_error(
                        "unable to idle Vulkan for terrain MSAA change");
                g_SceneRenderer.recreate(
                    {static_cast<std::uint32_t>(g_MainWindowData.Width),
                     static_cast<std::uint32_t>(g_MainWindowData.Height)},
                    g_MainWindowData.ImageCount,g_AtmosphereFrame.quality,
                    g_AtmosphereFrame.screen_resolution_divisor,
                    world_terrain_msaa);
            }
            if(!g_SceneRenderer.supports_terrain_msaa()){
                ImGui::EndDisabled();
                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(
                        "Requires 4x colour/depth samples and MAX depth resolve");
            }
            if(CheckboxWithHotkey("Capsule diagnostic","K",ImGuiKey_K,
                                  &world_show_capsule))
                overlay_dirty=true;
            if(CheckboxWithHotkey("Contact normal","N",ImGuiKey_N,
                                  &world_show_contact_normal))
                overlay_dirty=true;
            if(CheckboxWithHotkey("LOD zones","L",ImGuiKey_L,
                                  &show_camera_lod_zones))
                overlay_dirty=true;
            ImGui::SeparatorText("Sun");
            if(CheckboxWithHotkey("Animate sun","Y",ImGuiKey_Y,
                                  &world_animate_sun)){
                world_sun_orbit_azimuth=world_sun_azimuth;
                world_sun_orbit_phase=world_sun_elevation;
            }
            if(world_animate_sun){
                constexpr double minimum_cycle_seconds=0.25;
                constexpr double maximum_cycle_seconds=600.0;
                ImGui::SetNextItemWidth(190.0F);
                ImGui::SliderScalar("Cycle (seconds)",ImGuiDataType_Double,
                    &world_sun_cycle_seconds,
                    &minimum_cycle_seconds,&maximum_cycle_seconds,"%.1f");
                ImGui::Text("Azimuth %.1f deg",world_sun_azimuth*180.0F/
                            std::numbers::pi_v<float>);
                ImGui::Text("Elevation %.1f deg",world_sun_elevation*180.0F/
                            std::numbers::pi_v<float>);
                ImGui::TextDisabled("Fast live atmospheric shadows");
            }else{
                ImGui::SetNextItemWidth(190.0F);
                ImGui::SliderAngle(
                    "Azimuth",&world_sun_azimuth,-180.0F,180.0F);
                ImGui::SetNextItemWidth(190.0F);
                ImGui::SliderAngle(
                    "Elevation",&world_sun_elevation,-90.0F,90.0F);
            }
            if(ImGui::Button("Reset sun")){
                world_sun_azimuth=
                    tetra_viewer::default_world_sun_azimuth_radians;
                world_sun_elevation=
                    tetra_viewer::default_world_sun_elevation_radians;
                world_sun_orbit_azimuth=world_sun_azimuth;
                world_sun_orbit_phase=world_sun_elevation;
            }
            ImGui::SeparatorText("Atmosphere");
            CheckboxWithHotkey("Atmosphere","H",ImGuiKey_H,
                               &g_AtmosphereFrame.enabled);
            ImGui::SetNextItemWidth(190.0F);
            const auto transport_name=tetra_viewer::atmosphere_transport_name(
                g_AtmosphereFrame.transport);
            if(ImGui::BeginCombo("Transport",transport_name.data())){
                constexpr std::array transports{
                    tetra_viewer::AtmosphereTransport::qualified_baseline,
                    tetra_viewer::AtmosphereTransport::faithful_hillaire,
                    tetra_viewer::AtmosphereTransport::reference_hillaire_2020};
                for(const auto transport:transports){
                    const auto name=
                        tetra_viewer::atmosphere_transport_name(transport);
                    const bool selected=g_AtmosphereFrame.transport==transport;
                    if(ImGui::Selectable(name.data(),selected)){
                        g_AtmosphereFrame.transport=transport;
                        if(transport==tetra_viewer::AtmosphereTransport::
                                          reference_hillaire_2020)
                            g_AtmosphereFrame.rendering_method=
                                tetra_viewer::AtmosphereRenderingMethod::
                                    temporal_half_resolution;
                    }
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(190.0F);
            const auto rendering_method_name=
                tetra_viewer::atmosphere_rendering_method_name(
                    g_AtmosphereFrame.rendering_method);
            if(ImGui::BeginCombo("Atmosphere renderer",
                                 rendering_method_name.data())){
                constexpr std::array methods{
                    tetra_viewer::AtmosphereRenderingMethod::current_qualified,
                    tetra_viewer::AtmosphereRenderingMethod::native_screen_oracle,
                    tetra_viewer::AtmosphereRenderingMethod::deterministic_half_resolution,
                    tetra_viewer::AtmosphereRenderingMethod::temporal_half_resolution,
                    tetra_viewer::AtmosphereRenderingMethod::deterministic_shadowed_froxels};
                for(const auto method:methods){
                    const auto name=
                        tetra_viewer::atmosphere_rendering_method_name(method);
                    const bool selected=
                        g_AtmosphereFrame.rendering_method==method;
                    if(ImGui::Selectable(name.data(),selected))
                        g_AtmosphereFrame.rendering_method=method;
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(190.0F);
            const auto shadow_integrator_name=
                tetra_viewer::atmosphere_shadow_integrator_name(
                    g_AtmosphereFrame.shadow_integrator);
            if(ImGui::BeginCombo("Shadow integration",
                                 shadow_integrator_name.data())){
                constexpr std::array integrators{
                    tetra_viewer::AtmosphereShadowIntegrator::fixed_32,
                    tetra_viewer::AtmosphereShadowIntegrator::adaptive_transition,
                    tetra_viewer::AtmosphereShadowIntegrator::minmax_segments,
                    tetra_viewer::AtmosphereShadowIntegrator::moment_hybrid,
                    tetra_viewer::AtmosphereShadowIntegrator::epipolar_minmax};
                for(const auto integrator:integrators){
                    const auto name=
                        tetra_viewer::atmosphere_shadow_integrator_name(integrator);
                    const bool selected=
                        g_AtmosphereFrame.shadow_integrator==integrator;
                    if(ImGui::Selectable(name.data(),selected))
                        g_AtmosphereFrame.shadow_integrator=integrator;
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(190.0F);
            const auto surface_bias_name=
                tetra_viewer::surface_shadow_bias_mode_name(
                    g_AtmosphereFrame.surface_shadow_bias);
            if(ImGui::BeginCombo("Surface shadow bias",
                                 surface_bias_name.data())){
                constexpr std::array modes{
                    tetra_viewer::SurfaceShadowBiasMode::slope_scaled,
                    tetra_viewer::SurfaceShadowBiasMode::receiver_plane};
                for(const auto mode:modes){
                    const auto name=
                        tetra_viewer::surface_shadow_bias_mode_name(mode);
                    const bool selected=
                        g_AtmosphereFrame.surface_shadow_bias==mode;
                    if(ImGui::Selectable(name.data(),selected))
                        g_AtmosphereFrame.surface_shadow_bias=mode;
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(190.0F);
            const auto shadow_filter_name=
                tetra_viewer::atmosphere_shadow_filter_name(
                    g_AtmosphereFrame.shadow_filter);
            if(ImGui::BeginCombo("Shadow filtering",
                                 shadow_filter_name.data())){
                constexpr std::array filters{
                    tetra_viewer::AtmosphereShadowFilter::unfiltered,
                    tetra_viewer::AtmosphereShadowFilter::fixed_tent,
                    tetra_viewer::AtmosphereShadowFilter::physical_footprint};
                for(const auto filter:filters){
                    const auto name=
                        tetra_viewer::atmosphere_shadow_filter_name(filter);
                    const bool selected=g_AtmosphereFrame.shadow_filter==filter;
                    if(ImGui::Selectable(name.data(),selected))
                        g_AtmosphereFrame.shadow_filter=filter;
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(190.0F);
            const auto preset_name=tetra_viewer::atmosphere_preset_name(
                world_atmosphere_preset);
            if(ImGui::BeginCombo("Preset",preset_name.data())){
                constexpr std::array presets{
                    tetra_viewer::AtmospherePreset::gameplay_planet,
                    tetra_viewer::AtmospherePreset::earth,
                    tetra_viewer::AtmospherePreset::mars_like,
                    tetra_viewer::AtmospherePreset::dense_haze,
                    tetra_viewer::AtmospherePreset::nearly_airless};
                for(const auto preset:presets){
                    const auto name=tetra_viewer::atmosphere_preset_name(preset);
                    const bool selected=world_atmosphere_preset==preset;
                    if(ImGui::Selectable(name.data(),selected)){
                        const double metres_per_world_unit=
                            g_AtmosphereFrame.parameters.metres_per_world_unit;
                        g_AtmosphereFrame.parameters=
                            tetra_viewer::atmosphere_preset(preset);
                        g_AtmosphereFrame.parameters.metres_per_world_unit=
                            metres_per_world_unit;
                        if(preset==tetra_viewer::AtmospherePreset::gameplay_planet)
                            g_AtmosphereFrame.parameters=
                                tetra_viewer::adapt_compact_atmosphere_to_relief(
                                    g_AtmosphereFrame.parameters,
                                    world_maximum_terrain_relief_metres);
                        world_atmosphere_preset=preset;
                    }
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            bool physical_changed=false;
            if(ImGui::CollapsingHeader("Physical parameters")){
                const auto drag_double=[&](const char* label,double& value,
                                           double speed,double minimum,
                                           double maximum,const char* format){
                    return ImGui::DragScalar(label,ImGuiDataType_Double,&value,
                        static_cast<float>(speed),&minimum,&maximum,format,
                        ImGuiSliderFlags_AlwaysClamp|
                        ImGuiSliderFlags_Logarithmic);
                };
                physical_changed|=drag_double("Ground radius (m)",
                    g_AtmosphereFrame.parameters.ground_radius_metres,1000.0,
                    1000.0,1.0e9,"%.0f");
                physical_changed|=drag_double("Atmosphere height (m)",
                    g_AtmosphereFrame.parameters.atmosphere_height_metres,100.0,
                    100.0,1.0e7,"%.0f");
                physical_changed|=drag_double("Rayleigh height (m)",
                    g_AtmosphereFrame.parameters.rayleigh_scale_height_metres,
                    20.0,100.0,1.0e6,"%.0f");
                physical_changed|=drag_double("Aerosol height (m)",
                    g_AtmosphereFrame.parameters.mie_scale_height_metres,
                    10.0,10.0,1.0e6,"%.0f");
                const double coefficient_minimum=0.0;
                const double coefficient_maximum=1.0e-3;
                physical_changed|=ImGui::DragScalarN("Rayleigh scattering",
                    ImGuiDataType_Double,
                    g_AtmosphereFrame.parameters.rayleigh_scattering_per_metre.data(),
                    3,1.0e-7,&coefficient_minimum,&coefficient_maximum,"%.3e",
                    ImGuiSliderFlags_AlwaysClamp);
                physical_changed|=ImGui::DragScalarN("Aerosol scattering",
                    ImGuiDataType_Double,
                    g_AtmosphereFrame.parameters.mie_scattering_per_metre.data(),
                    3,1.0e-7,&coefficient_minimum,&coefficient_maximum,"%.3e",
                    ImGuiSliderFlags_AlwaysClamp);
                physical_changed|=ImGui::DragScalarN("Aerosol absorption",
                    ImGuiDataType_Double,
                    g_AtmosphereFrame.parameters.mie_absorption_per_metre.data(),
                    3,1.0e-7,&coefficient_minimum,&coefficient_maximum,"%.3e",
                    ImGuiSliderFlags_AlwaysClamp);
                physical_changed|=ImGui::DragScalarN("Upper-air absorption",
                    ImGuiDataType_Double,
                    g_AtmosphereFrame.parameters.absorption_per_metre.data(),
                    3,1.0e-7,&coefficient_minimum,&coefficient_maximum,"%.3e",
                    ImGuiSliderFlags_AlwaysClamp);
                const double anisotropy_minimum=-0.95;
                const double anisotropy_maximum=0.95;
                physical_changed|=ImGui::DragScalar("Aerosol anisotropy",
                    ImGuiDataType_Double,
                    &g_AtmosphereFrame.parameters.mie_anisotropy,0.005,
                    &anisotropy_minimum,&anisotropy_maximum,"%.3f",
                    ImGuiSliderFlags_AlwaysClamp);
                const double albedo_minimum=0.0;
                const double albedo_maximum=1.0;
                physical_changed|=ImGui::DragScalarN("Ground albedo",
                    ImGuiDataType_Double,
                    g_AtmosphereFrame.parameters.ground_albedo.data(),3,0.005,
                    &albedo_minimum,&albedo_maximum,"%.3f",
                    ImGuiSliderFlags_AlwaysClamp);
                const double altitude_minimum=0.0;
                const double altitude_maximum=1.0e7;
                physical_changed|=ImGui::DragScalar("Absorption peak (m)",
                    ImGuiDataType_Double,
                    &g_AtmosphereFrame.parameters.absorption_peak_altitude_metres,
                    100.0F,&altitude_minimum,&altitude_maximum,"%.0f",
                    ImGuiSliderFlags_AlwaysClamp);
                physical_changed|=drag_double("Absorption width (m)",
                    g_AtmosphereFrame.parameters.absorption_half_width_metres,
                    100.0,1.0,1.0e7,"%.0f");
                const double irradiance_minimum=0.0;
                const double irradiance_maximum=20.0;
                physical_changed|=ImGui::DragScalarN("Solar irradiance",
                    ImGuiDataType_Double,
                    g_AtmosphereFrame.parameters.solar_irradiance.data(),3,0.01,
                    &irradiance_minimum,&irradiance_maximum,"%.3f",
                    ImGuiSliderFlags_AlwaysClamp);
                const double sun_radius_minimum=0.0001;
                const double sun_radius_maximum=0.1;
                physical_changed|=ImGui::DragScalar("Solar radius (rad)",
                    ImGuiDataType_Double,
                    &g_AtmosphereFrame.parameters.solar_angular_radius_radians,
                    0.00005,&sun_radius_minimum,&sun_radius_maximum,"%.5f",
                    ImGuiSliderFlags_AlwaysClamp);
            }
            if(physical_changed)
                world_atmosphere_preset=
                    tetra_viewer::AtmospherePreset::custom;
            if(ImGui::CollapsingHeader("Atmosphere quality")){
                constexpr std::array<const char*,3> quality_names{
                    "Low","Default","High"};
                int quality_index=static_cast<int>(g_AtmosphereFrame.quality);
                ImGui::SetNextItemWidth(190.0F);
                if(ImGui::Combo("Profile",&quality_index,quality_names.data(),
                                static_cast<int>(quality_names.size()))){
                    g_AtmosphereFrame.quality=
                        static_cast<tetra_viewer::AtmosphereQuality>(quality_index);
                    if(vkDeviceWaitIdle(g_Device)!=VK_SUCCESS)
                        throw std::runtime_error(
                            "unable to idle Vulkan for atmosphere quality change");
                    g_SceneRenderer.recreate(
                        {static_cast<std::uint32_t>(g_MainWindowData.Width),
                         static_cast<std::uint32_t>(g_MainWindowData.Height)},
                        g_MainWindowData.ImageCount,g_AtmosphereFrame.quality,
                        g_AtmosphereFrame.screen_resolution_divisor,
                        world_terrain_msaa);
                }
                constexpr std::array<const char*,3> resolution_names{
                    "Half","One third","One quarter"};
                int resolution_index=static_cast<int>(
                    g_AtmosphereFrame.screen_resolution_divisor)-2;
                ImGui::SetNextItemWidth(190.0F);
                if(ImGui::Combo("Screen integration",&resolution_index,
                                resolution_names.data(),
                                static_cast<int>(resolution_names.size()))){
                    g_AtmosphereFrame.screen_resolution_divisor=
                        static_cast<std::uint32_t>(resolution_index+2);
                    if(vkDeviceWaitIdle(g_Device)!=VK_SUCCESS)
                        throw std::runtime_error(
                            "unable to idle Vulkan for atmosphere resolution change");
                    g_SceneRenderer.recreate(
                        {static_cast<std::uint32_t>(g_MainWindowData.Width),
                         static_cast<std::uint32_t>(g_MainWindowData.Height)},
                        g_MainWindowData.ImageCount,g_AtmosphereFrame.quality,
                        g_AtmosphereFrame.screen_resolution_divisor,
                        world_terrain_msaa);
                }
                const double distance_minimum=10'000.0;
                const double distance_maximum=10'000'000.0;
                ImGui::DragScalar("Aerial range (m)",ImGuiDataType_Double,
                    &g_AtmosphereFrame.maximum_aerial_distance_metres,1000.0,
                    &distance_minimum,&distance_maximum,"%.0f",
                    ImGuiSliderFlags_AlwaysClamp|
                    ImGuiSliderFlags_Logarithmic);
            }
            if(ImGui::CollapsingHeader("Atmosphere diagnostics")){
                constexpr std::array<const char*,28> debug_names{
                    "Final composition","Transmittance lookup",
                    "Multiple scattering lookup","Sky-view lookup",
                    "Aerial scattering slice","Aerial transmittance slice",
                    "Reversed depth","Shadow cascade 0","Shadow cascade 1",
                    "Shadow cascade 2","Shadow cascade 3",
                    "Long-path shadow coverage","Long-path direct loss",
                    "Full-sky before terrain shadow",
                    "Full-sky after terrain shadow",
                    "Receiver-fitted atmosphere shadow",
                    "Full-resolution direct scattering",
                    "Full-resolution multiple scattering",
                    "Direct scattering after terrain shadow",
                    "HDR terrain before atmosphere",
                    "Surface-truncated direct after shadow",
                    "Surface-truncated multiple scattering",
                    "Raw directional shadow loss",
                    "Long-shadow epipolar classification",
                    "Long-shadow epipolar traversal",
                    "Terrain direct-shadow visibility",
                    "Terrain indirect lighting",
                    "Terrain direct lighting"};
                ImGui::SetNextItemWidth(190.0F);
                ImGui::Combo("Debug view",&g_AtmosphereFrame.debug_view,
                             debug_names.data(),
                             static_cast<int>(debug_names.size()));
                const auto& timings=g_SceneRenderer.gpu_timings();
                if(timings.valid){
                    ImGui::Text("GPU shadow %.2f ms  atmosphere %.2f ms",
                                timings.shadows_milliseconds,
                                timings.atmosphere_milliseconds);
                    ImGui::Text("GPU terrain %.2f ms  composite %.2f ms",
                                timings.terrain_milliseconds,
                                timings.composite_milliseconds);
                    ImGui::Text("GPU depth %.2f  integrate %.2f  temporal %.2f ms",
                                timings.depth_reduction_milliseconds,
                                timings.screen_integration_milliseconds,
                                timings.temporal_reconstruction_milliseconds);
                }else ImGui::TextDisabled("GPU timings pending");
                ImGui::Text("Atmosphere allocation %.1f MiB",
                    static_cast<double>(
                        g_SceneRenderer.atmosphere_allocation_bytes())/
                        (1024.0*1024.0));
                ImGui::Text("HDR/depth allocation %.1f MiB",
                    static_cast<double>(
                        g_SceneRenderer.scene_target_allocation_bytes())/
                        (1024.0*1024.0));
                const auto& dispatches=
                    g_SceneRenderer.atmosphere_dispatch_counts();
                ImGui::Text("LUT dispatch T %llu M %llu S %llu I %llu A %llu L %llu",
                    static_cast<unsigned long long>(dispatches.transmittance),
                    static_cast<unsigned long long>(
                        dispatches.multiple_scattering),
                    static_cast<unsigned long long>(dispatches.sky_view),
                    static_cast<unsigned long long>(dispatches.sky_irradiance),
                    static_cast<unsigned long long>(
                        dispatches.aerial_perspective),
                    static_cast<unsigned long long>(dispatches.long_shadow));
            }
            ImGui::SetNextItemWidth(190.0F);
            const double exposure_minimum=-6.0;
            const double exposure_maximum=6.0;
            ImGui::SliderScalar("Exposure (EV)",ImGuiDataType_Double,
                &world_exposure_ev,&exposure_minimum,
                &exposure_maximum,"%.2f");
            g_AtmosphereFrame.exposure=static_cast<float>(
                std::exp2(world_exposure_ev));
            controls_hovered=ImGui::IsWindowHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            ImGui::End();
        }
        // Let Dear ImGui reserve pointer input over the floating controls;
        // otherwise use raw GLFW drag state for dependable orbiting.
        // The wireframe regression view must remain deterministic for Vulkan
        // screenshot validation; do not let residual trackpad-wheel inertia
        // move its camera immediately after launch.
        const auto& input=ImGui::GetIO();
        const bool camera_input_allowed=tetra_viewer::manipulator_pointer_input_allowed(
            deterministic_visual_check,controls_hovered,input.WantCaptureMouse);
        if(world_mode){
            const auto now=std::chrono::steady_clock::now();
            const double elapsed=std::chrono::duration<double>(
                now-previous_world_frame).count();
            previous_world_frame=now;
            if(world_animate_sun){
                world_sun_orbit_phase=
                    tetra_viewer::advance_world_sun_orbit_phase(
                        world_sun_orbit_phase,elapsed,
                        world_sun_cycle_seconds);
                const auto angles=tetra_viewer::world_sun_orbit_angles(
                    world_sun_orbit_azimuth,world_sun_orbit_phase);
                world_sun_azimuth=static_cast<float>(angles.azimuth_radians);
                world_sun_elevation=static_cast<float>(
                    angles.elevation_radians);
            }
            if(glfwGetKey(window,GLFW_KEY_ESCAPE)==GLFW_PRESS&&world_pointer_captured){
                world_pointer_captured=false;
                glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_NORMAL);
            }
            if(tetra_viewer::world_pointer_capture_on_click(
                   world_gpu_automation_requested,world_pointer_captured,
                   glfwGetMouseButton(window,GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS,
                   controls_hovered)){
                world_pointer_captured=true;
                glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_DISABLED);
                glfwGetCursorPos(window,&world_cursor_x,&world_cursor_y);
            }
            double cursor_x{},cursor_y{};
            glfwGetCursorPos(window,&cursor_x,&cursor_y);
            bool world_camera_look_changed=false;
            if(world_pointer_captured){
                const double delta_x=cursor_x-world_cursor_x;
                const double delta_y=cursor_y-world_cursor_y;
                world_camera_look_changed=delta_x!=0.0||delta_y!=0.0;
                world_controller.look(delta_x,delta_y);
            }
            world_cursor_x=cursor_x;world_cursor_y=cursor_y;
            tetra_viewer::FirstPersonInput movement;
            double world_free_fly_vertical=0.0;
            if(!input.WantCaptureKeyboard||world_pointer_captured){
                movement.forward=(glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS?1.0:0.0)-
                    (glfwGetKey(window,GLFW_KEY_S)==GLFW_PRESS?1.0:0.0);
                movement.right=(glfwGetKey(window,GLFW_KEY_D)==GLFW_PRESS?1.0:0.0)-
                    (glfwGetKey(window,GLFW_KEY_A)==GLFW_PRESS?1.0:0.0);
                movement.sprint=glfwGetKey(window,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS||
                    glfwGetKey(window,GLFW_KEY_RIGHT_SHIFT)==GLFW_PRESS;
                movement.super_speed=
                    glfwGetKey(window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS||
                    glfwGetKey(window,GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
                movement.jump=glfwGetKey(window,GLFW_KEY_SPACE)==GLFW_PRESS;
            }
            if(world_gpu_motion_applied&&world_gpu_walk_steps_remaining!=0U)
                movement.forward=1.0;
            if(world_free_fly){
                world_free_fly_vertical=
                    (glfwGetKey(window,GLFW_KEY_SPACE)==GLFW_PRESS?1.0:0.0)-
                    (glfwGetKey(window,GLFW_KEY_C)==GLFW_PRESS?1.0:0.0);
                auto direction=world_controller.forward()*movement.forward+
                    world_controller.right()*movement.right+
                    tetra::Vec3{0.0,world_free_fly_vertical,0.0};
                const double magnitude=std::sqrt(direction.x*direction.x+
                    direction.y*direction.y+direction.z*direction.z);
                if(magnitude>1.0)direction=direction/magnitude;
                const tetra_viewer::FirstPersonConfiguration movement_configuration;
                const double speed=movement_configuration.walk_speed*
                    tetra_viewer::movement_speed_multiplier(
                        movement,movement_configuration);
                if(!world_paused||world_single_step){
                    const bool automated_walk=world_gpu_motion_applied&&
                        world_gpu_walk_steps_remaining!=0U;
                    world_controller.state().feet=world_controller.state().feet+
                        direction*speed*(automated_walk?1.0/120.0:
                            (world_single_step?1.0/120.0:elapsed));
                    if(automated_walk)--world_gpu_walk_steps_remaining;
                }
                world_controller.state().velocity={};
                world_controller.state().grounded=false;
            }else if(!world_paused||world_single_step){
                world_controller.advance(
                    world_gpu_motion_applied&&
                        world_gpu_walk_steps_remaining!=0U?
                        1.0/120.0:
                        (world_single_step?1.0/120.0:elapsed),movement,
                    sphere);
                if(world_gpu_motion_applied&&
                   world_gpu_walk_steps_remaining!=0U)
                    --world_gpu_walk_steps_remaining;
            }
            world_single_step=false;
            // LOD error is measured in rendered pixels, not logical UI points.
            // On a Retina display DisplaySize is half the framebuffer size;
            // using it here made the terrain roughly twice as coarse as the
            // configured pixel target after the first frame.
            const double framebuffer_height=static_cast<double>(
                std::max(g_MainWindowData.Height,1));
            camera=world_controller.camera(
                framebuffer_height,
                static_cast<double>(std::max(g_MainWindowData.Width,1))/
                    framebuffer_height);
            const bool lod_camera_locked=
                world_free_fly&&world_lock_lod_camera;
            const auto lod_camera=tetra_viewer::resolve_world_lod_camera(
                camera,lod_camera_locked,world_lod_camera);
            const bool lod_camera_interactive=!lod_camera_locked&&
                (movement.forward!=0.0||movement.right!=0.0||
                 world_free_fly_vertical!=0.0||
                 (!world_free_fly&&!world_controller.state().grounded)||
                 world_camera_look_changed);
            if(world_runtime)
                world_runtime->set_camera(lod_camera,lod_camera_interactive);
            view_camera_position=world_controller.eye_position();
            if(world_show_capsule||world_show_contact_normal)overlay_dirty=true;
        }else{
        bool lod_camera_moved=false;
        if(!input.WantCaptureKeyboard){
            const bool command=input.KeyCtrl||input.KeySuper;
            auto keyboard_mode=camera_gizmo_mode;
            if(glfwGetKey(window,GLFW_KEY_Q)==GLFW_PRESS)
                keyboard_mode=tetra_viewer::CameraGizmoMode::select;
            else if(glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS)
                keyboard_mode=tetra_viewer::CameraGizmoMode::translate;
            else if(glfwGetKey(window,GLFW_KEY_E)==GLFW_PRESS)
                keyboard_mode=tetra_viewer::CameraGizmoMode::rotate;
            if(keyboard_mode!=camera_gizmo_mode){camera_gizmo_mode=keyboard_mode;upload_dirty=true;}
            if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::select)
                camera_manipulator.preferred=tetra_viewer::CameraHandle::none;
            if(ImGui::IsKeyPressed(ImGuiKey_1)){
                camera_manipulator.space=tetra_viewer::ManipulatorSpace::world;upload_dirty=true;
            }
            if(ImGui::IsKeyPressed(ImGuiKey_2)){
                camera_manipulator.space=tetra_viewer::ManipulatorSpace::local;upload_dirty=true;
            }
            const auto preferred_axis=[&](std::size_t axis){
                return tetra_viewer::preferred_axis_handle(camera_gizmo_mode,axis);
            };
            const auto previous_preferred=camera_manipulator.preferred;
            if(ImGui::IsKeyPressed(ImGuiKey_X))camera_manipulator.preferred=preferred_axis(0U);
            if(ImGui::IsKeyPressed(ImGuiKey_Y))camera_manipulator.preferred=preferred_axis(1U);
            if(ImGui::IsKeyPressed(ImGuiKey_Z)&&!command)
                camera_manipulator.preferred=preferred_axis(2U);
            if(camera_manipulator.preferred!=previous_preferred)upload_dirty=true;
            if(ImGui::IsKeyPressed(ImGuiKey_Escape)&&camera_manipulator.dragging()){
                static_cast<void>(camera_manipulator.cancel_drag(lod_camera_pose));
                lod_reconcile_pending=lod_reconcile_before_drag;
                lod_camera_pose.apply(camera);upload_dirty=true;
            }
            if(command&&ImGui::IsKeyPressed(ImGuiKey_Z)){
                const bool changed=input.KeyShift?camera_manipulator.redo(lod_camera_pose):
                    camera_manipulator.undo(lod_camera_pose);
                if(changed){lod_camera_pose.apply(camera);lod_reconcile_pending=true;upload_dirty=true;}
            }
        }
        if(camera_input_allowed){
            double cursor_x{},cursor_y{};
            glfwGetCursorPos(window,&cursor_x,&cursor_y);
            const bool left_pressed=
                glfwGetMouseButton(window,GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
            const bool left_started=left_pressed&&!previous_left_pressed;
            const bool shift_pressed=
                glfwGetKey(window,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS||
                glfwGetKey(window,GLFW_KEY_RIGHT_SHIFT)==GLFW_PRESS;
            const bool alt_pressed=
                glfwGetKey(window,GLFW_KEY_LEFT_ALT)==GLFW_PRESS||
                glfwGetKey(window,GLFW_KEY_RIGHT_ALT)==GLFW_PRESS;
            const double precision=shift_pressed?0.15:1.0;
            const double viewport_width=std::max(1.0F,input.DisplaySize.x);
            const double viewport_height=std::max(1.0F,input.DisplaySize.y);
            const auto view_forward=orbit_camera.forward();
            const auto view_right=orbit_camera.right();
            const auto view_up=orbit_camera.up();
            const tetra_viewer::ManipulatorView manipulator_view{
                view_camera_position,view_forward,view_right,view_up,
                camera.vertical_fov_radians,viewport_width,viewport_height};
            const auto project=[&](tetra::Vec3 point){
                return tetra_viewer::project_to_vulkan_viewport(point,
                    view_camera_position,view_forward,view_right,view_up,
                    camera.vertical_fov_radians,viewport_width,viewport_height);
            };
            const auto segment_distance=[](double x,double y,
                                           const tetra_viewer::ViewportPoint& first,
                                           const tetra_viewer::ViewportPoint& second){
                if(!first.visible||!second.visible)return std::numeric_limits<double>::infinity();
                const double dx=second.x-first.x,dy=second.y-first.y;
                const double length_squared=dx*dx+dy*dy;
                const double amount=length_squared>1.0e-12?std::clamp(
                    ((x-first.x)*dx+(y-first.y)*dy)/length_squared,0.0,1.0):0.0;
                return std::hypot(x-(first.x+amount*dx),y-(first.y+amount*dy));
            };
            const auto camera_screen=project(lod_camera_pose.position);
            const auto camera_frustum=tetra_viewer::build_lod_camera_frustum(
                lod_camera_pose,camera,manipulator_view);
            const auto camera_pick_distance=[&](){
                double distance=std::hypot(cursor_x-camera_screen.x,
                                           cursor_y-camera_screen.y);
                for(const auto& segment:camera_frustum.segments)
                    distance=std::min(distance,segment_distance(cursor_x,cursor_y,
                        project(segment.first),project(segment.second)));
                return distance;
            };
            const auto handle_geometry=tetra_viewer::build_camera_handle_geometry(
                lod_camera_pose,camera_gizmo_mode,camera_manipulator.space,
                manipulator_view);
            const auto previous_hover=camera_manipulator.hovered;
            if(lod_camera_selected&&!camera_manipulator.dragging())
                camera_manipulator.hovered=camera_manipulator.preferred!=
                    tetra_viewer::CameraHandle::none?camera_manipulator.preferred:
                    tetra_viewer::hit_test_camera_handles(
                        handle_geometry,manipulator_view,cursor_x,cursor_y).handle;
            else if(!camera_manipulator.dragging())
                camera_manipulator.hovered=tetra_viewer::CameraHandle::none;
            if(camera_manipulator.hovered!=previous_hover)upload_dirty=true;
            if(camera_manipulator.hovered!=tetra_viewer::CameraHandle::none||
               camera_manipulator.dragging())
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if(left_started){
                previous_cursor_x=cursor_x;
                previous_cursor_y=cursor_y;
                if(alt_pressed){
                    empty_viewport_gesture.cancel();
                    camera_drag_mode=shift_pressed?CameraDragMode::pan:CameraDragMode::orbit;
                }else{
                    const auto picked=lod_camera_selected?camera_manipulator.hovered:
                        tetra_viewer::CameraHandle::none;
                    if(picked!=tetra_viewer::CameraHandle::none&&
                       camera_gizmo_mode!=tetra_viewer::CameraGizmoMode::select){
                        empty_viewport_gesture.cancel();
                        lod_reconcile_before_drag=lod_reconcile_pending;
                        static_cast<void>(camera_manipulator.begin_drag(
                            picked,lod_camera_pose,manipulator_view,cursor_x,cursor_y));
                        upload_dirty=true;
                    }else if(camera_screen.visible&&camera_pick_distance()<=10.0){
                        empty_viewport_gesture.cancel();
                        lod_camera_selected=true;
                        upload_dirty=true;
                    }else{
                        empty_viewport_gesture.begin(
                            lod_camera_selected,cursor_x,cursor_y);
                        // A primary-button drag on empty space orbits the
                        // editor view while preserving the selected camera.
                        // Only a stationary release becomes deselection.
                        camera_drag_mode=shift_pressed?CameraDragMode::pan:
                            CameraDragMode::orbit;
                    }
                }
            }
            if(left_pressed){
                if(camera_manipulator.dragging()){
                    lod_camera_moved=camera_manipulator.update_drag(
                        lod_camera_pose,manipulator_view,cursor_x,cursor_y,precision);
                }else if(camera_drag_mode!=CameraDragMode::none){
                    empty_viewport_gesture.update(cursor_x,cursor_y);
                    const double delta_x=cursor_x-previous_cursor_x;
                    const double delta_y=cursor_y-previous_cursor_y;
                    if(camera_drag_mode==CameraDragMode::orbit)
                        orbit_camera.orbit(delta_x,delta_y,precision);
                    else orbit_camera.pan(delta_x,delta_y,viewport_height,
                                          camera.vertical_fov_radians,precision);
                    if(delta_x!=0.0||delta_y!=0.0)overlay_dirty=true;
                }
            }else{
                if(previous_left_pressed&&camera_manipulator.dragging()){
                    static_cast<void>(camera_manipulator.finish_drag(lod_camera_pose));
                    upload_dirty=true;
                }
                if(previous_left_pressed&&
                   empty_viewport_gesture.finish_should_deselect()){
                    lod_camera_selected=false;
                    camera_manipulator.hovered=tetra_viewer::CameraHandle::none;
                    upload_dirty=true;
                }
                camera_drag_mode=CameraDragMode::none;
            }
            if(left_pressed){previous_cursor_x=cursor_x;previous_cursor_y=cursor_y;}
            previous_left_pressed=left_pressed;
            const float wheel=input.MouseWheel!=0.0f?input.MouseWheel:
                (shift_pressed?input.MouseWheelH:0.0f);
            if(wheel!=0.0f){orbit_camera.dolly(wheel,precision);overlay_dirty=true;}
        }else{
            camera_drag_mode=CameraDragMode::none;
            empty_viewport_gesture.cancel();
            previous_left_pressed=false;
        }
        if(lod_camera_moved){
            lod_camera_pose.apply(camera);
            has_adaptive_result=false;
            lod_reconcile_pending=true;
            upload_dirty=true;
        }
        }
        // During a gizmo drag, publish small complete conforming transactions
        // and coalesce all newer pointer poses at the next slice boundary.
        // Releasing the gizmo immediately replaces the relaxed request with a
        // full-quality one that runs to convergence in the same worker.
        const bool interactive_lod_update=camera_manipulator.dragging();
        if(!world_mode&&lod_reconcile_pending&&
           (interactive_lod_update||!previous_left_pressed)){
            const bool background_supported=
                mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green&&
                (adaptation_configuration.lod_update==
                     tetra::LodUpdateStrategy::transactional_active_cut||
                 adaptation_configuration.lod_update==
                     tetra::LodUpdateStrategy::saturated_clusters);
            if(background_supported){
                const auto intent=interactive_lod_update
                    ?tetra_viewer::MeshUpdateIntent::interactive_camera
                    :tetra_viewer::MeshUpdateIntent::settled;
                const auto parameters=mesh_update_parameters(intent);
                const bool request_changed=!submitted_mesh_update||
                    submitted_mesh_operation!=
                        tetra_viewer::MeshUpdateOperation::reconcile_lod||
                    submitted_mesh_revision!=mesh.revision()||
                    !tetra_viewer::same_mesh_update_parameters(
                        *submitted_mesh_update,parameters);
                const bool intent_changed=submitted_mesh_update&&
                    submitted_mesh_update->intent!=intent;
                if(tetra_viewer::should_submit_mesh_update(
                       mesh_update_in_flight,request_changed,
                       interactive_lod_update,
                       mesh_slice_published_this_frame,intent_changed)){
                    submitted_mesh_request_id=
                        mesh_update_worker.submit(mesh,parameters);
                    submitted_mesh_update=parameters;
                    submitted_mesh_operation=
                        tetra_viewer::MeshUpdateOperation::reconcile_lod;
                    submitted_mesh_revision=mesh.revision();
                    mesh_update_in_flight=true;
                }
            }else if(!interactive_lod_update){
                if(mesh_update_in_flight){
                    mesh_update_worker.cancel();
                    mesh_update_in_flight=false;
                    submitted_mesh_update.reset();
                    submitted_mesh_request_id=0U;
                }
                const auto start=std::chrono::steady_clock::now();
                last_adaptive_result=reconcile_to_current_surface();
                last_refine_milliseconds=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-start).count();
                has_adaptive_result=true;
                refined=true;
                mesh_validation_current=false;
                upload_dirty=true;
                lod_reconcile_pending=!reconcile_complete;
            }
        }
        if(!world_mode)update_orbit_camera();
        // Derive the view direction from the orbit angles rather than
        // target-position subtraction. At distance zero both positions are
        // equal, but the camera must retain a well-defined orientation.
        const float aspect = ImGui::GetIO().DisplaySize.x / std::max(1.0F, ImGui::GetIO().DisplaySize.y);
        const auto projection=tetra_viewer::make_infinite_reversed_projection(
            view_camera_position,prepared_scene.render_origin,
            world_mode?world_controller.forward():orbit_camera.forward(),
            {0.0,1.0,0.0},camera.vertical_fov_radians,aspect);
        const tetra::Vec3 f=projection.forward;
        const tetra::Vec3 right=projection.right;
        const tetra::Vec3 up=projection.up;
        const tetra::Vec3 camera_position=projection.camera_relative;
        std::copy(projection.matrix.begin(),projection.matrix.end(),
                  g_CameraData.begin());
        const auto sun=tetra_viewer::world_sun_direction(
            world_sun_azimuth,world_sun_elevation);
        g_AtmosphereFrame.camera_relative_world=camera_position;
        g_AtmosphereFrame.camera_right=projection.right;
        g_AtmosphereFrame.camera_down=projection.up;
        g_AtmosphereFrame.camera_forward=projection.forward;
        g_AtmosphereFrame.sun_direction=sun;
        g_AtmosphereFrame.dynamic_sun=world_animate_sun;
        g_AtmosphereFrame.vertical_tangent=projection.tangent;
        g_AtmosphereFrame.aspect_ratio=projection.aspect_ratio;
        const double planet_radius_world=
            g_AtmosphereFrame.parameters.ground_radius_metres/
            g_AtmosphereFrame.parameters.metres_per_world_unit;
        const tetra::Vec3 planet_centre_world{
            0.5,0.5-planet_radius_world,0.5};
        g_AtmosphereFrame.planet_centre_relative_world=
            planet_centre_world-prepared_scene.render_origin;
        g_AtmosphereFrame.shadow_front=nullptr;
        if(world_runtime&&g_AtmosphereFrame.enabled){
            const double metres=
                g_AtmosphereFrame.parameters.metres_per_world_unit;
            const auto camera_from_centre=
                (g_AtmosphereFrame.camera_relative_world-
                 g_AtmosphereFrame.planet_centre_relative_world)*metres;
            const double altitude=std::sqrt(
                camera_from_centre.x*camera_from_centre.x+
                camera_from_centre.y*camera_from_centre.y+
                camera_from_centre.z*camera_from_centre.z)-
                g_AtmosphereFrame.parameters.ground_radius_metres;
            const double aerial=tetra_viewer::atmosphere_local_aerial_distance(
                g_AtmosphereFrame.parameters,altitude,
                g_AtmosphereFrame.maximum_aerial_distance_metres);
            const auto quality=tetra_viewer::atmosphere_quality_settings(
                g_AtmosphereFrame.quality);
            if(world_animate_sun){
                // A sun-specific CPU residency front cannot converge while
                // its direction changes every frame. The renderer uses a live
                // fitted GPU preview over the published terrain instead of
                // superseding background terrain work. A complete residency
                // front is requested immediately when animation stops.
                // Preserve the last settled caster-residency request while
                // the renderer changes its live fitted projection. This keeps
                // the off-screen mountain blocks that were already selected
                // instead of immediately collapsing to visible geometry and
                // producing a detached atmospheric-shadow silhouette.
                world_runtime->set_atmosphere_shadow_request(
                    world_retained_atmosphere_shadow_request);
            }else{
                const auto cascades=tetra_viewer::make_stable_shadow_cascades(
                    camera_position,projection.forward,sun,
                    quality.shadow_resolution);
                const double receiver_distance=std::clamp(
                    aerial/std::max(metres,1.0e-12),
                    cascades.cascades.back().split_distance,2048.0);
                auto current_view=
                    tetra_viewer::make_atmosphere_shadow_front_request(
                        view_camera_position,projection.forward,projection.right,
                        projection.up,projection.tangent,projection.aspect_ratio,
                        receiver_distance,1.0,sun,receiver_distance,
                        prepared_scene.render_origin,1U);
                current_view.map_resolution=
                    quality.atmosphere_shadow_resolution;
                if(!world_retained_atmosphere_shadow_request||
                   !tetra_viewer::atmosphere_shadow_request_covers_rotation(
                       *world_retained_atmosphere_shadow_request,current_view)){
                    world_retained_atmosphere_shadow_request=
                        tetra_viewer::make_atmosphere_shadow_front_request(
                            view_camera_position,projection.forward,
                            projection.right,projection.up,projection.tangent,
                            projection.aspect_ratio,receiver_distance,1.15,sun,
                            receiver_distance,prepared_scene.render_origin,1U);
                    world_retained_atmosphere_shadow_request->map_resolution=
                        quality.atmosphere_shadow_resolution;
                }
                world_runtime->set_atmosphere_shadow_request(
                    world_retained_atmosphere_shadow_request);
                const auto& front=world_runtime->atmosphere_shadow_front();
                // A complete front is only complete for the camera footprint
                // it was built from. During walking the runtime deliberately
                // keeps presenting the previous terrain while its replacement
                // shadow front is planned. Do not label that old front as
                // current: its loss field can exceed an unrelated sky lookup
                // and clamp the whole sky to black.
                if(tetra_viewer::atmosphere_shadow_front_covers_view(
                        front,current_view))
                    g_AtmosphereFrame.shadow_front=&*front;
            }
        }
        const tetra::Vec3 light=shading_model==tetra_viewer::ShadingModel::stone_pbr?
            sun:tetra::Vec3{-f.x,-f.y,-f.z};
        const std::array<float,12> render_parameters{
            static_cast<float>(light.x),static_cast<float>(light.y),
            static_cast<float>(light.z),show_volume_edges ? 1.0F : 0.0F,
            static_cast<float>(shading_model), show_surface_edges ? 1.0F : 0.0F,
            show_faces ? 1.0F : 0.0F,
            x_cutaway ? x_cut_position-static_cast<float>(prepared_scene.render_origin.x)
                      :2.0F,
            static_cast<float>(camera_position.x),
            static_cast<float>(camera_position.y),
            static_cast<float>(camera_position.z),
            world_mode&&world_smooth_normals?1.0F:0.0F};
        std::copy(render_parameters.begin(),render_parameters.end(),
                  g_CameraData.begin()+16);
        if (upload_dirty||overlay_dirty) {
            // Editor overlays deliberately do not include mesh surface-edge
            // segments. The mesh wireframe has its own depth-tested native
            // line pass; mixing it here would recreate the see-through bug.
            std::vector<tetra_viewer::SceneVertex> overlay_lines;
            const auto add_overlay_line=[&](tetra::Vec3 first,tetra::Vec3 second,
                                            std::array<float,3> colour){
                const auto vertex=[&](tetra::Vec3 point){
                    tetra_viewer::SceneVertex result;
                    point=point-prepared_scene.render_origin;
                    result.position[0]=static_cast<float>(point.x);
                    result.position[1]=static_cast<float>(point.y);
                    result.position[2]=static_cast<float>(point.z);
                    result.colour[0]=colour[0];result.colour[1]=colour[1];result.colour[2]=colour[2];
                    return result;
                };
                overlay_lines.push_back(vertex(first));
                overlay_lines.push_back(vertex(second));
            };
            if(world_mode&&(world_show_capsule||world_show_contact_normal)){
                const auto& player=world_controller.state();
                if(world_show_capsule){
                    constexpr std::size_t segments=24U;
                    constexpr double radius=0.025;
                    constexpr double height=0.16;
                    for(double y:std::array{radius,height-radius})
                        for(std::size_t index=0;index<segments;++index){
                            const double first=2.0*std::acos(-1.0)*index/segments;
                            const double second=2.0*std::acos(-1.0)*(index+1U)/segments;
                            add_overlay_line(
                                player.feet+tetra::Vec3{radius*std::cos(first),y,
                                                        radius*std::sin(first)},
                                player.feet+tetra::Vec3{radius*std::cos(second),y,
                                                        radius*std::sin(second)},
                                {0.95F,0.78F,0.18F});
                        }
                    for(double angle:std::array{0.0,std::acos(-1.0)*0.5,
                                                std::acos(-1.0),std::acos(-1.0)*1.5})
                        add_overlay_line(
                            player.feet+tetra::Vec3{radius*std::cos(angle),radius,
                                                    radius*std::sin(angle)},
                            player.feet+tetra::Vec3{radius*std::cos(angle),height-radius,
                                                    radius*std::sin(angle)},
                            {0.95F,0.78F,0.18F});
                }
                if(world_show_contact_normal)
                    add_overlay_line(player.feet+tetra::Vec3{0.0,0.003,0.0},
                        player.feet+player.contact_normal*0.12,
                        {0.18F,0.86F,0.96F});
            }
            if(show_camera_lod_zones){
                if(world_mode&&world_runtime){
                    for(const auto& line:world_runtime->lod_zone_lines())
                        add_overlay_line(line.first,line.second,line.colour);
                }else{
                const auto retained_epoch=[&](tetra::TetId owner){
                    while(true){
                        const auto depth=tetra::tet_depth(owner);
                        if(depth<adaptation_planning_cache.camera_temporal_layers.size()){
                            const auto& layer=
                                adaptation_planning_cache.camera_temporal_layers[depth];
                            const auto found=std::lower_bound(
                                layer.addresses.begin(),layer.addresses.end(),owner);
                            if(found!=layer.addresses.end()&&*found==owner){
                                const auto index=static_cast<std::size_t>(
                                    found-layer.addresses.begin());
                                return adaptation_planning_cache.camera_demand_epoch-
                                    layer.last_demand_epochs[index]<=
                                    adaptation_configuration.recent_retention_epochs;
                            }
                        }
                        if(depth<3U)break;
                        owner=tetra::make_tet_id(
                            tetra::tet_root(owner),tetra::tet_path(owner)>>3U);
                    }
                    return false;
                };
                const auto zone_colour=[](tetra::CameraLodZone zone){
                    switch(zone){
                        case tetra::CameraLodZone::visible:
                            return std::array<float,3>{0.96F,0.98F,1.0F};
                        case tetra::CameraLodZone::near:
                            return std::array<float,3>{0.12F,0.88F,0.92F};
                        case tetra::CameraLodZone::guard:
                            return std::array<float,3>{0.98F,0.82F,0.12F};
                        case tetra::CameraLodZone::recent:
                            return std::array<float,3>{0.88F,0.30F,0.92F};
                        case tetra::CameraLodZone::predicted:
                            return std::array<float,3>{1.0F,0.46F,0.10F};
                        case tetra::CameraLodZone::cold:
                            return std::array<float,3>{0.18F,0.32F,0.72F};
                    }
                    return std::array<float,3>{0.7F,0.7F,0.7F};
                };
                constexpr std::array<std::array<std::size_t,2>,6> edges{{
                    {{0U,1U}},{{0U,2U}},{{0U,3U}},
                    {{1U,2U}},{{1U,3U}},{{2U,3U}}}};
                for(const auto owner:mesh.logical_red_owners()){
                    auto zone=tetra::camera_lod_demand(
                        mesh,owner,camera,adaptation_configuration).zone;
                    if(zone==tetra::CameraLodZone::cold&&
                       (adaptation_configuration.camera_lod_policy==
                            tetra::CameraLodPolicy::guarded_recent||
                        adaptation_configuration.camera_lod_policy==
                            tetra::CameraLodPolicy::guarded_predicted)&&
                       retained_epoch(owner))
                        zone=tetra::CameraLodZone::recent;
                    const auto colour=zone_colour(zone);
                    const auto& tet=mesh.tetrahedron(owner);
                    for(const auto edge:edges)
                        add_overlay_line(mesh.vertices()[tet.vertices[edge[0]]],
                                         mesh.vertices()[tet.vertices[edge[1]]],
                                         colour);
                }
                }
            }
            if(!world_mode&&(!deterministic_visual_check||manipulator_visual_check)){
                const tetra_viewer::ManipulatorView manipulator_view{
                    view_camera_position,f,right,up,camera.vertical_fov_radians,
                    std::max(1.0F,input.DisplaySize.x),
                    std::max(1.0F,input.DisplaySize.y)};
                const auto frustum=tetra_viewer::build_lod_camera_frustum(
                    lod_camera_pose,camera,manipulator_view);
                const std::array<float,3> camera_colour=lod_camera_selected?
                    std::array<float,3>{0.42F,0.86F,0.56F}:
                    std::array<float,3>{0.86F,0.88F,0.94F};
                for(const auto& segment:frustum.segments)
                    add_overlay_line(segment.first,segment.second,camera_colour);

                if(lod_camera_selected&&
                   camera_gizmo_mode!=tetra_viewer::CameraGizmoMode::select){
                    const auto geometry=tetra_viewer::build_camera_handle_geometry(
                        lod_camera_pose,camera_gizmo_mode,camera_manipulator.space,
                        manipulator_view);
                    const auto colour=[&](tetra_viewer::CameraHandle handle){
                        std::array<float,3> result{0.9F,0.9F,0.9F};
                        switch(handle){
                            case tetra_viewer::CameraHandle::move_x:
                            case tetra_viewer::CameraHandle::rotate_x:
                                result={0.86F,0.14F,0.16F};break;
                            case tetra_viewer::CameraHandle::move_y:
                            case tetra_viewer::CameraHandle::rotate_y:
                                result={0.18F,0.76F,0.22F};break;
                            case tetra_viewer::CameraHandle::move_z:
                            case tetra_viewer::CameraHandle::rotate_z:
                                result={0.16F,0.40F,0.92F};break;
                            case tetra_viewer::CameraHandle::move_xy:
                                result={0.90F,0.82F,0.15F};break;
                            case tetra_viewer::CameraHandle::move_xz:
                                result={0.88F,0.24F,0.92F};break;
                            case tetra_viewer::CameraHandle::move_yz:
                                result={0.18F,0.86F,0.92F};break;
                            case tetra_viewer::CameraHandle::rotate_arcball:
                                result={0.68F,0.72F,0.78F};break;
                            default:break;
                        }
                        if(handle==camera_manipulator.hovered||
                           handle==camera_manipulator.preferred||
                           handle==camera_manipulator.active)
                            result={1.0F,0.86F,0.02F};
                        else if(camera_manipulator.dragging())
                            for(float& value:result)value*=0.38F;
                        return result;
                    };
                    for(const auto& segment:geometry.segments)
                        add_overlay_line(segment.first,segment.second,
                                         colour(segment.handle));
                    // Cone faces are filled below in screen space. Drawing
                    // every facet edge here exposes rear faces and can read as
                    // a second nested cone at oblique angles.
                    for(const auto& quad:geometry.quads){
                        const auto tint=colour(quad.handle);
                        for(std::size_t index=0;index<4U;++index)
                            add_overlay_line(quad.points[index],
                                quad.points[(index+1U)%4U],tint);
                    }
                    constexpr std::size_t ring_segments=96U;
                    for(const auto& ring:geometry.rings){
                        for(std::size_t index=0;index<ring_segments;++index){
                            const double a=2.0*std::acos(-1.0)*index/ring_segments;
                            const double b=2.0*std::acos(-1.0)*(index+1U)/ring_segments;
                            const auto point=[&](double angle){return ring.centre+
                                (ring.first_basis*std::cos(angle)+
                                 ring.second_basis*std::sin(angle))*ring.radius;};
                            const auto first=point(a),second=point(b);
                            auto tint=colour(ring.handle);
                            const auto middle=(first+second)*0.5-ring.centre;
                            const auto toward_view=view_camera_position-ring.centre;
                            const double facing=middle.x*toward_view.x+
                                middle.y*toward_view.y+middle.z*toward_view.z;
                            if(facing<0.0&&ring.handle!=camera_manipulator.active)
                                for(float& value:tint)value*=0.42F;
                            add_overlay_line(first,second,tint);
                        }
                    }
                }
            }
            if(upload_dirty){
                if(world_mode&&world_runtime&&
                   world_runtime->retained_surface()!=nullptr)
                    g_SceneRenderer.upload_surface_ranges(
                        *world_runtime->retained_surface(),
                        prepared_scene.hierarchy_line_vertices,overlay_lines);
                else if(retained_surface_upload_ready)
                    g_SceneRenderer.upload_surface_ranges(
                        surface_host_staging,prepared_scene.hierarchy_line_vertices,
                        overlay_lines);
                else g_SceneRenderer.upload(prepared_scene.triangle_vertices,
                                            prepared_scene.hierarchy_line_vertices,
                                            overlay_lines);
                if(retained_upload_check&&retained_surface_upload_ready)
                    retained_upload_present_pending=true;
            }else g_SceneRenderer.upload_editor_lines(overlay_lines);
            upload_dirty = false;
            overlay_dirty = false;
        }

        if((!deterministic_visual_check||manipulator_visual_check)&&lod_camera_selected&&
           camera_gizmo_mode==tetra_viewer::CameraGizmoMode::translate){
            const tetra_viewer::ManipulatorView label_view{
                view_camera_position,f,right,up,camera.vertical_fov_radians,
                std::max(1.0F,input.DisplaySize.x),std::max(1.0F,input.DisplaySize.y)};
            const auto geometry=tetra_viewer::build_camera_handle_geometry(
                lod_camera_pose,camera_gizmo_mode,camera_manipulator.space,label_view);
            const auto projected=[&](tetra::Vec3 value){
                return tetra_viewer::project_to_vulkan_viewport(
                    value,label_view.position,label_view.forward,label_view.right,
                    label_view.up,label_view.vertical_fov_radians,
                    label_view.viewport_width,label_view.viewport_height);
            };
            const auto fill_colour=[&](tetra_viewer::CameraHandle handle){
                if(handle==camera_manipulator.hovered||
                   handle==camera_manipulator.preferred||handle==camera_manipulator.active)
                    return IM_COL32(255,220,5,255);
                switch(handle){
                    case tetra_viewer::CameraHandle::move_x:return IM_COL32(219,36,41,255);
                    case tetra_viewer::CameraHandle::move_y:return IM_COL32(46,194,56,255);
                    case tetra_viewer::CameraHandle::move_z:return IM_COL32(41,102,235,255);
                    case tetra_viewer::CameraHandle::move_xy:return IM_COL32(230,209,38,255);
                    case tetra_viewer::CameraHandle::move_xz:return IM_COL32(224,61,235,255);
                    case tetra_viewer::CameraHandle::move_yz:return IM_COL32(46,219,235,255);
                    default:return IM_COL32(230,230,230,110);
                }
            };
            struct FilledHandleTriangle {
                std::array<tetra_viewer::ViewportPoint,3> points;
                ImU32 colour;
                double depth;
            };
            std::vector<FilledHandleTriangle> fills;
            fills.reserve(geometry.triangles.size());
            const auto cone_handle=[](tetra_viewer::CameraHandle handle){
                return handle==tetra_viewer::CameraHandle::move_x||
                    handle==tetra_viewer::CameraHandle::move_y||
                    handle==tetra_viewer::CameraHandle::move_z;
            };
            auto light=label_view.up*0.65+label_view.right*0.25-label_view.forward*0.55;
            const double light_length=std::sqrt(light.x*light.x+light.y*light.y+
                                                light.z*light.z);
            light=light/light_length;
            for(const auto& triangle:geometry.triangles){
                std::array<tetra_viewer::ViewportPoint,3> points{};
                for(std::size_t index=0;index<3U;++index)
                    points[index]=projected(triangle.points[index]);
                if(!std::ranges::all_of(points,[](const auto& point){return point.visible;}))
                    continue;
                auto tint=fill_colour(triangle.handle);
                if(cone_handle(triangle.handle)){
                    const auto first=triangle.points[1]-triangle.points[0];
                    const auto second=triangle.points[2]-triangle.points[0];
                    tetra::Vec3 normal{
                        first.y*second.z-first.z*second.y,
                        first.z*second.x-first.x*second.z,
                        first.x*second.y-first.y*second.x};
                    const double normal_length=std::sqrt(normal.x*normal.x+normal.y*normal.y+
                                                         normal.z*normal.z);
                    normal=normal/normal_length;
                    const auto centre=(triangle.points[0]+triangle.points[1]+
                                       triangle.points[2])/3.0;
                    const auto toward_view=label_view.position-centre;
                    if(normal.x*toward_view.x+normal.y*toward_view.y+
                       normal.z*toward_view.z<=0.0)
                        continue;
                    const double facing=std::max(0.0,
                        normal.x*light.x+normal.y*light.y+normal.z*light.z);
                    const float brightness=static_cast<float>(0.56+0.44*facing);
                    auto colour=ImGui::ColorConvertU32ToFloat4(tint);
                    colour.x*=brightness;colour.y*=brightness;colour.z*=brightness;
                    tint=ImGui::ColorConvertFloat4ToU32(colour);
                }
                fills.push_back({points,tint,
                    (points[0].depth+points[1].depth+points[2].depth)/3.0});
            }
            std::ranges::sort(fills,std::greater{},&FilledHandleTriangle::depth);
            // Background draw commands are composited above the Vulkan scene
            // but before Dear ImGui windows. Manipulator fills therefore stay
            // visible in the viewport without bleeding across the controls.
            auto* viewport_draw_list=ImGui::GetBackgroundDrawList();
            for(const auto& fill:fills)
                viewport_draw_list->AddTriangleFilled(
                    ImVec2(static_cast<float>(fill.points[0].x),
                           static_cast<float>(fill.points[0].y)),
                    ImVec2(static_cast<float>(fill.points[1].x),
                           static_cast<float>(fill.points[1].y)),
                    ImVec2(static_cast<float>(fill.points[2].x),
                           static_cast<float>(fill.points[2].y)),fill.colour);
            for(const auto& quad:geometry.quads){
                if(!quad.filled)continue;
                std::array<tetra_viewer::ViewportPoint,4> points{};
                for(std::size_t index=0;index<4U;++index)
                    points[index]=projected(quad.points[index]);
                if(!std::ranges::all_of(points,[](const auto& point){return point.visible;}))
                    continue;
                viewport_draw_list->AddQuadFilled(
                    ImVec2(static_cast<float>(points[0].x),static_cast<float>(points[0].y)),
                    ImVec2(static_cast<float>(points[1].x),static_cast<float>(points[1].y)),
                    ImVec2(static_cast<float>(points[2].x),static_cast<float>(points[2].y)),
                    ImVec2(static_cast<float>(points[3].x),static_cast<float>(points[3].y)),
                    fill_colour(quad.handle));
            }
        }

        // Rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            ++world_gpu_rendered_frames;
            const bool capture_front_ready=world_runtime&&
                tetra_viewer::world_capture_front_ready(
                    world_runtime->diagnostics());
            world_gpu_capture_ready_frames=capture_front_ready?
                world_gpu_capture_ready_frames+1U:0U;
            const std::size_t capture_warmup_frames=
                g_AtmosphereFrame.transport==
                        tetra_viewer::AtmosphereTransport::
                        reference_hillaire_2020||
                    g_AtmosphereFrame.rendering_method==
                        tetra_viewer::AtmosphereRenderingMethod::
                        temporal_half_resolution?64U:1U;
            const bool requested_frame_ready=
                world_gpu_capture_frame_target!=0U&&
                world_gpu_rendered_frames>=world_gpu_capture_frame_target;
            const bool static_capture=
                world_gpu_walk_steps==0U&&world_gpu_look_x==0.0&&
                world_gpu_look_y==0.0;
            const bool motion_capture=world_gpu_motion_applied&&
                ((world_gpu_capture_after_motion_frames!=0U&&
                  world_gpu_capture_motion_frame_count>=
                      world_gpu_capture_after_motion_frames)||
                 (world_gpu_capture_after_motion_frames==0U&&
                  world_gpu_motion_saw_busy&&
                  world_gpu_walk_steps_remaining==0U));
            const bool settled_capture_ready=
                world_gpu_capture_frame_target==0U&&
                world_gpu_capture_ready_frames>=capture_warmup_frames&&
                (static_capture||motion_capture);
            g_AtmosphereFrame.capture_requested=
                !world_gpu_atmosphere_capture_path.empty()&&
                !world_gpu_atmosphere_capture_submitted&&world_runtime&&
                (requested_frame_ready||settled_capture_ready);
            if(g_AtmosphereFrame.capture_requested)
                world_gpu_atmosphere_capture_submitted=true;
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            FrameRender(wd, draw_data);
            FramePresent(wd);
            if(retained_upload_present_pending){
                const auto& upload=g_SceneRenderer.surface_upload_metrics();
                std::cout<<"{\"event\":\"vulkan_retained_upload\","
                         <<"\"source_generation\":"<<upload.source_generation
                         <<",\"full_reallocation\":"
                         <<(upload.full_reallocation?"true":"false")
                         <<",\"uploaded_bytes\":"<<upload.uploaded_bytes
                         <<",\"upload_ranges\":"<<upload.upload_ranges
                         <<",\"reused_ranges\":"<<upload.reused_ranges
                         <<",\"draw_calls\":"<<upload.draw_calls<<"}\n";
                retained_upload_present_pending=false;
                glfwSetWindowShouldClose(window,GLFW_TRUE);
            }
            if(world_gpu_atmosphere_benchmark&&world_runtime&&
               !world_runtime->diagnostics().busy){
                const auto& timing=g_SceneRenderer.gpu_timings();
                if(timing.valid){
                    if(world_gpu_atmosphere_resize_check&&
                       !world_gpu_atmosphere_resize_requested){
                        world_gpu_pre_resize_scene_bytes=
                            g_SceneRenderer.scene_target_allocation_bytes();
                        world_gpu_atmosphere_resize_requested=true;
                        glfwSetWindowSize(window,900,600);
                        continue;
                    }
                    if(world_gpu_atmosphere_resize_check&&
                       g_SceneRenderer.scene_target_allocation_bytes()==
                           world_gpu_pre_resize_scene_bytes)
                        continue;
                    const std::array values{
                        timing.shadows_milliseconds,
                        timing.atmosphere_milliseconds,
                        timing.terrain_milliseconds,
                        timing.depth_reduction_milliseconds,
                        timing.screen_integration_milliseconds,
                        timing.temporal_reconstruction_milliseconds,
                        timing.composite_milliseconds};
                    constexpr std::size_t warmup_frames=8U;
                    constexpr std::size_t measured_frames=31U;
                    if(world_gpu_benchmark_warmup_frames<warmup_frames){
                        for(std::size_t pass=0;pass<values.size();++pass)
                            world_gpu_refresh_maximum[pass]=std::max(
                                world_gpu_refresh_maximum[pass],values[pass]);
                        ++world_gpu_benchmark_warmup_frames;
                        continue;
                    }
                    for(std::size_t pass=0;pass<values.size();++pass)
                        world_gpu_benchmark_samples[pass].push_back(values[pass]);
                    if(world_gpu_benchmark_samples[0].size()<measured_frames)
                        continue;
                    std::array<tetra_viewer::ScalarSampleSummary,7> summaries{};
                    for(std::size_t pass=0;pass<summaries.size();++pass)
                        summaries[pass]=*tetra_viewer::summarize_samples(
                            world_gpu_benchmark_samples[pass]);
                    const auto write_pass=[&](std::string_view name,
                                               std::size_t pass){
                        std::cout<<"\""<<name<<"\":{\"median_ms\":"
                            <<summaries[pass].median<<",\"p95_ms\":"
                            <<summaries[pass].percentile_95
                            <<",\"maximum_ms\":"<<summaries[pass].maximum
                            <<",\"initial_refresh_maximum_ms\":"
                            <<world_gpu_refresh_maximum[pass]<<'}';
                    };
                    std::cout<<"{\"event\":\"gpu_atmosphere_benchmark\","
                        <<"\"sample_count\":"<<measured_frames<<','
                        <<"\"warmup_frames\":"<<warmup_frames<<','
                        <<"\"rendering_method\":\""
                        <<tetra_viewer::atmosphere_rendering_method_name(
                              g_AtmosphereFrame.rendering_method)
                        <<"\",\"terrain_msaa\":"
                        <<(world_terrain_msaa?"true":"false")
                        <<",\"screen_resolution_divisor\":"
                        <<g_AtmosphereFrame.screen_resolution_divisor<<','
                        <<"\"shadows_ms\":"<<summaries[0].median<<','
                        <<"\"atmosphere_ms\":"<<summaries[1].median<<','
                        <<"\"terrain_ms\":"<<summaries[2].median<<','
                        <<"\"depth_reduction_ms\":"<<summaries[3].median<<','
                        <<"\"screen_integration_ms\":"<<summaries[4].median<<','
                        <<"\"temporal_reconstruction_ms\":"
                        <<summaries[5].median<<','
                        <<"\"composite_ms\":"<<summaries[6].median<<',';
                    write_pass("shadows",0U);std::cout<<',';
                    write_pass("atmosphere",1U);std::cout<<',';
                    write_pass("terrain",2U);std::cout<<',';
                    write_pass("depth_reduction",3U);std::cout<<',';
                    write_pass("screen_integration",4U);std::cout<<',';
                    write_pass("temporal_reconstruction",5U);std::cout<<',';
                    write_pass("composite",6U);std::cout<<',';
                    std::cout
                        <<"\"atmosphere_bytes\":"
                        <<g_SceneRenderer.atmosphere_allocation_bytes()<<','
                        <<"\"scene_target_bytes\":"
                        <<g_SceneRenderer.scene_target_allocation_bytes()<<',';
                    const auto& fitted_shadow=
                        g_SceneRenderer.atmosphere_shadow_map_status();
                    const auto runtime_shadow=world_runtime->diagnostics();
                    std::cout<<"\"atmosphere_shadow\":{\"revision\":"
                        <<fitted_shadow.revision<<",\"refreshes\":"
                        <<fitted_shadow.refreshes<<",\"integrator\":\""
                        <<tetra_viewer::atmosphere_shadow_integrator_name(
                              fitted_shadow.integrator)
                        <<"\",\"depth_generation\":"
                        <<fitted_shadow.depth_generation
                        <<",\"hierarchy_generation\":"
                        <<fitted_shadow.hierarchy_generation
                        <<",\"hierarchy_complete\":"
                        <<(fitted_shadow.hierarchy_complete?"true":"false")
                        <<",\"integration_fallback\":"
                        <<(fitted_shadow.integration_fallback?"true":"false")
                        <<",\"caster_draws\":"
                        <<fitted_shadow.caster_draws
                        <<",\"local_depth_world_spans\":["
                        <<fitted_shadow.local_depth_world_spans[0]<<','
                        <<fitted_shadow.local_depth_world_spans[1]<<','
                        <<fitted_shadow.local_depth_world_spans[2]<<','
                        <<fitted_shadow.local_depth_world_spans[3]<<']'
                        <<",\"local_texel_world_sizes\":["
                        <<fitted_shadow.local_texel_world_sizes[0]<<','
                        <<fitted_shadow.local_texel_world_sizes[1]<<','
                        <<fitted_shadow.local_texel_world_sizes[2]<<','
                        <<fitted_shadow.local_texel_world_sizes[3]<<']'
                        <<",\"local_comparison_biases_normalized\":["
                        <<fitted_shadow.local_comparison_biases_normalized[0]<<','
                        <<fitted_shadow.local_comparison_biases_normalized[1]<<','
                        <<fitted_shadow.local_comparison_biases_normalized[2]<<','
                        <<fitted_shadow.local_comparison_biases_normalized[3]<<']'
                        <<",\"local_comparison_biases_world\":["
                        <<fitted_shadow.local_comparison_biases_world[0]<<','
                        <<fitted_shadow.local_comparison_biases_world[1]<<','
                        <<fitted_shadow.local_comparison_biases_world[2]<<','
                        <<fitted_shadow.local_comparison_biases_world[3]<<']'
                        <<",\"depth_world_span\":"
                        <<fitted_shadow.fitted_depth_world_span
                        <<",\"texel_world_size_x\":"
                        <<fitted_shadow.fitted_texel_world_size_x
                        <<",\"texel_world_size_y\":"
                        <<fitted_shadow.fitted_texel_world_size_y
                        <<",\"comparison_bias_normalized\":"
                        <<fitted_shadow.comparison_bias_normalized
                        <<",\"comparison_bias_world\":"
                        <<fitted_shadow.comparison_bias_world
                        <<",\"raster_bias_constant\":"
                        <<fitted_shadow.raster_bias_constant
                        <<",\"raster_bias_slope\":"
                        <<fitted_shadow.raster_bias_slope
                        <<",\"epipolar_radial_resolution\":"
                        <<fitted_shadow.epipolar_radial_resolution
                        <<",\"epipolar_angular_rows\":"
                        <<fitted_shadow.epipolar_angular_rows
                        <<",\"epipolar_elements\":"
                        <<fitted_shadow.epipolar_elements
                        <<",\"epipolar_visited_nodes\":"
                        <<fitted_shadow.epipolar_visited_nodes
                        <<",\"epipolar_emitted_intervals\":"
                        <<fitted_shadow.epipolar_emitted_intervals
                        <<",\"epipolar_fallbacks\":"
                        <<fitted_shadow.epipolar_fallbacks
                        <<",\"epipolar_overflows\":"
                        <<fitted_shadow.epipolar_overflows
                        <<",\"epipolar_hierarchy_refreshes\":"
                        <<fitted_shadow.epipolar_hierarchy_refreshes
                        <<",\"complete\":"
                        <<(fitted_shadow.complete?"true":"false")
                        <<",\"front_publications\":"
                        <<runtime_shadow.atmosphere_shadow_publications
                        <<",\"front_cancellations\":"
                        <<runtime_shadow.atmosphere_shadow_cancellations
                        <<",\"front_planning_ms\":"
                        <<runtime_shadow.atmosphere_shadow_planning_milliseconds
                        <<",\"terrain_busy\":"
                        <<(runtime_shadow.busy?"true":"false")
                        <<",\"terrain_converged\":"
                        <<(runtime_shadow.converged?"true":"false")
                        <<",\"terrain_budget_exceeded\":"
                        <<(runtime_shadow.budget_exceeded?"true":"false")
                        <<",\"terrain_logical_cells\":"
                        <<runtime_shadow.logical_cells
                        <<"},"
                        <<"\"dispatches\":{"
                        <<"\"transmittance\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().transmittance
                        <<",\"multiple_scattering\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().multiple_scattering
                        <<",\"sky_view\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().sky_view
                        <<",\"sky_irradiance\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().sky_irradiance
                        <<",\"aerial_perspective\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().aerial_perspective
                        <<",\"long_shadow\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().long_shadow
                        <<",\"screen_reconstruction\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().screen_reconstruction
                        <<",\"temporal_history_accepts\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().temporal_history_accepts
                        <<",\"temporal_history_invalidations\":"
                        <<g_SceneRenderer.atmosphere_dispatch_counts().temporal_history_invalidations
                        <<"},"
                        <<"\"resize_checked\":"
                        <<(world_gpu_atmosphere_resize_check?"true":"false")
                        <<"}\n";
                    world_gpu_atmosphere_benchmark=false;
                }
            }
            if(world_gpu_atmosphere_probe&&world_runtime&&
               !world_runtime->diagnostics().busy&&
               g_SceneRenderer.latest_atmosphere_probe().valid){
                const auto& probe=g_SceneRenderer.latest_atmosphere_probe();
                tetra_viewer::AtmosphereNumericProbeValues actual{};
                for(std::size_t value=0;value<probe.values.size();++value)
                    for(std::size_t channel=0;channel<4U;++channel)
                        actual[value][channel]=probe.values[value][channel];
                const double metres=
                    g_AtmosphereFrame.parameters.metres_per_world_unit;
                const auto camera_from_centre=
                    (g_AtmosphereFrame.camera_relative_world-
                     g_AtmosphereFrame.planet_centre_relative_world)*metres;
                const double camera_altitude=std::sqrt(
                    camera_from_centre.x*camera_from_centre.x+
                    camera_from_centre.y*camera_from_centre.y+
                    camera_from_centre.z*camera_from_centre.z)-
                    g_AtmosphereFrame.parameters.ground_radius_metres;
                const double aerial_distance=
                    tetra_viewer::atmosphere_local_aerial_distance(
                        g_AtmosphereFrame.parameters,camera_altitude,
                        g_AtmosphereFrame.maximum_aerial_distance_metres);
                const tetra_viewer::AtmosphereNumericProbeInput input{
                    .parameters=g_AtmosphereFrame.parameters,
                    .camera_position_from_planet_centre_metres=
                        camera_from_centre,
                    .camera_right=g_AtmosphereFrame.camera_right,
                    .camera_down=g_AtmosphereFrame.camera_down,
                    .camera_forward=g_AtmosphereFrame.camera_forward,
                    .sun_direction=g_AtmosphereFrame.sun_direction,
                    .vertical_tangent=g_AtmosphereFrame.vertical_tangent,
                    .aspect_ratio=g_AtmosphereFrame.aspect_ratio,
                    .maximum_aerial_distance_metres=aerial_distance,
                    .quality=g_AtmosphereFrame.quality};
                const auto report=tetra_viewer::evaluate_atmosphere_numeric_probe(
                    actual,input);
                const auto write_value=[](double value){
                    if(std::isfinite(value))std::cout<<value;
                    else std::cout<<"null";
                };
                const auto write_vector=[&](const auto& values){
                    std::cout<<'[';
                    for(std::size_t channel=0;channel<values.size();++channel){
                        if(channel!=0U)std::cout<<',';
                        write_value(values[channel]);
                    }
                    std::cout<<']';
                };
                std::cout<<"{\"event\":\"gpu_atmosphere_probe\",\"status\":\""
                         <<(report.passed?"pass":"fail")
                         <<"\",\"comparisons\":[";
                for(std::size_t index=0;index<report.comparisons.size();++index){
                    if(index!=0U)std::cout<<',';
                    const auto& comparison=report.comparisons[index];
                    std::cout<<"{\"stage\":\""<<comparison.name
                             <<"\",\"pass\":"
                             <<(comparison.passed?"true":"false")
                             <<",\"actual\":";
                    write_vector(comparison.actual);
                    std::cout<<",\"expected\":";
                    write_vector(comparison.expected);
                    std::cout<<",\"absolute_error\":";
                    write_vector(comparison.absolute_error);
                    std::cout<<",\"relative_error\":";
                    write_vector(comparison.relative_error);
                    std::cout<<'}';
                }
                std::cout<<"]}\n";
                world_process_exit_code=report.passed?0:3;
                world_gpu_atmosphere_probe=false;
            }
            if(world_gpu_shadow_projection_probe&&world_runtime&&
               !world_runtime->diagnostics().busy&&
               g_SceneRenderer.latest_atmosphere_probe().valid){
                const auto& actual=
                    g_SceneRenderer.latest_atmosphere_probe().values;
                const auto cascades=tetra_viewer::make_stable_shadow_cascades(
                    g_AtmosphereFrame.camera_relative_world,
                    g_AtmosphereFrame.camera_forward,
                    g_AtmosphereFrame.sun_direction,
                    tetra_viewer::atmosphere_quality_settings(
                        g_AtmosphereFrame.quality).shadow_resolution);
                const auto cases=
                    tetra_viewer::make_atmosphere_shadow_projection_probe_cases(
                        cascades);
                bool passed=true;
                double maximum_error{};
                std::cout<<"{\"event\":\"gpu_shadow_projection_probe\","
                         <<"\"cases\":[";
                for(std::size_t index=0;index<cases.size();++index){
                    double case_error{};
                    const std::array<double,3> expected{
                        cases[index].expected_clip.x,
                        cases[index].expected_clip.y,
                        cases[index].expected_clip.z};
                    for(std::size_t axis=0;axis<3U;++axis)
                        case_error=std::max(case_error,
                            std::abs(static_cast<double>(actual[index][axis])-
                                     expected[axis]));
                    const bool actual_sampleable=actual[index][3]>0.5F;
                    const bool case_passed=case_error<=2.0e-5&&
                        actual_sampleable==cases[index].expected_sampleable;
                    maximum_error=std::max(maximum_error,case_error);
                    passed=passed&&case_passed;
                    if(index!=0U)std::cout<<',';
                    std::cout<<"{\"cascade\":"<<cases[index].cascade
                             <<",\"pass\":"
                             <<(case_passed?"true":"false")
                             <<",\"actual\":["<<actual[index][0]<<','
                             <<actual[index][1]<<','<<actual[index][2]
                             <<"],\"expected\":["<<expected[0]<<','
                             <<expected[1]<<','<<expected[2]
                             <<"],\"sampleable\":"
                             <<(actual_sampleable?"true":"false")<<'}';
                }
                std::cout<<"],\"maximum_error\":"<<maximum_error
                         <<",\"status\":\""<<(passed?"pass":"fail")
                         <<"\"}\n";
                world_process_exit_code=passed?0:3;
                world_gpu_shadow_projection_probe=false;
                g_AtmosphereFrame.shadow_projection_probe_requested=false;
            }
            if(world_gpu_atmosphere_capture_submitted&&
               g_SceneRenderer.latest_capture().valid){
                const auto& capture=g_SceneRenderer.latest_capture();
                tetra_viewer::Rgb8Image image;
                std::vector<std::uint8_t> depth_mask,clear_mask,
                    silhouette_mask,outer_silhouette_mask,horizon_mask;
                std::string image_error;
                auto depth_mask_path=std::filesystem::path(
                    world_gpu_atmosphere_capture_path);
                depth_mask_path.replace_extension(".depth.pgm");
                auto clear_mask_path=std::filesystem::path(
                    world_gpu_atmosphere_capture_path);
                clear_mask_path.replace_extension(".clear.pgm");
                auto silhouette_mask_path=std::filesystem::path(
                    world_gpu_atmosphere_capture_path);
                silhouette_mask_path.replace_extension(".silhouette.pgm");
                auto horizon_mask_path=std::filesystem::path(
                    world_gpu_atmosphere_capture_path);
                horizon_mask_path.replace_extension(".horizon.pgm");
                if(!tetra_viewer::make_rgb8_image(
                       capture.pixels,capture.width,capture.height,
                       capture.bgra,false,image,image_error)||
                   !tetra_viewer::write_ppm(
                       world_gpu_atmosphere_capture_path,image,image_error)||
                   !tetra_viewer::make_reversed_depth_mask(
                       capture.reversed_depth,capture.width,capture.height,
                       depth_mask,image_error)||
                   !tetra_viewer::write_pgm(
                       depth_mask_path.string(),capture.width,capture.height,
                       depth_mask,image_error)||
                   !tetra_viewer::make_complement_mask(
                       depth_mask,clear_mask,image_error)||
                   !tetra_viewer::write_pgm(
                       clear_mask_path.string(),capture.width,capture.height,
                       clear_mask,image_error)||
                   !tetra_viewer::make_silhouette_band_mask(
                       depth_mask,capture.width,capture.height,3U,
                       silhouette_mask,image_error)||
                   !tetra_viewer::make_outer_silhouette_band_mask(
                       depth_mask,capture.width,capture.height,12U,
                       outer_silhouette_mask,image_error)||
                   !tetra_viewer::write_pgm(
                       silhouette_mask_path.string(),capture.width,
                       capture.height,silhouette_mask,image_error)||
                   !tetra_viewer::make_horizontal_band_mask(
                       capture.width,capture.height,0.5,0.2,
                       horizon_mask,image_error)||
                   !tetra_viewer::write_pgm(
                       horizon_mask_path.string(),capture.width,capture.height,
                       horizon_mask,image_error)){
                    std::cerr<<"could not write GPU atmosphere capture: "
                             <<image_error<<'\n';
                    world_process_exit_code=2;
                }else{
                    const auto analysis=tetra_viewer::analyse_rgb8_image(image);
                    const auto outer_limb_analysis=
                        tetra_viewer::analyse_rgb8_image(
                            image,outer_silhouette_mask);
                    std::size_t blue_outer_limb_pixels{};
                    for(std::size_t pixel=0;
                        pixel<outer_silhouette_mask.size();++pixel){
                        if(outer_silhouette_mask[pixel]==0U)continue;
                        const auto offset=pixel*3U;
                        const int red=image.pixels[offset];
                        const int green=image.pixels[offset+1U];
                        const int blue=image.pixels[offset+2U];
                        if(blue>=red+2&&blue>=green+2)
                            ++blue_outer_limb_pixels;
                    }
                    const auto projected_sun=projection.project(
                        projection.camera_relative+sun);
                    std::uint32_t sun_pixel_x{},sun_pixel_y{};
                    bool sun_centre_geometry_occluded=false;
                    if(projected_sun.visible){
                        sun_pixel_x=static_cast<std::uint32_t>(std::clamp(
                            std::llround((projected_sun.ndc_x*0.5+0.5)*
                                         static_cast<double>(capture.width-1U)),
                            0LL,static_cast<long long>(capture.width-1U)));
                        sun_pixel_y=static_cast<std::uint32_t>(std::clamp(
                            std::llround((projected_sun.ndc_y*0.5+0.5)*
                                         static_cast<double>(capture.height-1U)),
                            0LL,static_cast<long long>(capture.height-1U)));
                        sun_centre_geometry_occluded=depth_mask[
                            static_cast<std::size_t>(sun_pixel_y)*capture.width+
                            sun_pixel_x]!=0U;
                    }
                    const auto quality_name=[&]{
                        switch(g_AtmosphereFrame.quality){
                        case tetra_viewer::AtmosphereQuality::low:return "low";
                        case tetra_viewer::AtmosphereQuality::standard:
                            return "default";
                        case tetra_viewer::AtmosphereQuality::high:return "high";
                        }
                        return "unknown";
                    }();
                    std::cout<<"{\"event\":\"gpu_atmosphere_capture\","
                             <<"\"path\":\""
                             <<world_gpu_atmosphere_capture_path
                             <<"\",\"depth_mask_path\":\""
                             <<depth_mask_path.string()
                             <<"\",\"clear_mask_path\":\""
                             <<clear_mask_path.string()
                             <<"\",\"silhouette_mask_path\":\""
                             <<silhouette_mask_path.string()
                             <<"\",\"horizon_mask_path\":\""
                             <<horizon_mask_path.string()
                             <<"\",\"width\":"<<capture.width
                             <<",\"height\":"<<capture.height
                             <<",\"rendered_frame\":"
                             <<world_gpu_rendered_frames
                             <<",\"rgb_hash\":"
                             <<tetra_viewer::rgb8_hash(image)
                             <<",\"transport\":\""
                             <<tetra_viewer::atmosphere_transport_name(
                                   g_AtmosphereFrame.transport)
                             <<"\",\"rendering_method\":\""
                             <<tetra_viewer::atmosphere_rendering_method_name(
                                   g_AtmosphereFrame.rendering_method)
                             <<"\",\"screen_resolution_divisor\":"
                             <<g_AtmosphereFrame.screen_resolution_divisor
                             <<",\"shadow_integrator\":\""
                             <<tetra_viewer::atmosphere_shadow_integrator_name(
                                   g_AtmosphereFrame.shadow_integrator)
                             <<"\",\"quality\":\""<<quality_name
                             <<"\",\"preset\":\""
                             <<tetra_viewer::atmosphere_preset_name(
                                   world_atmosphere_preset)
                             <<"\",\"atmosphere_parameters\":{\"maximum_relief_metres\":"
                             <<world_maximum_terrain_relief_metres
                             <<",\"height_metres\":"
                             <<g_AtmosphereFrame.parameters.
                                   atmosphere_height_metres
                             <<",\"rayleigh_scale_height_metres\":"
                             <<g_AtmosphereFrame.parameters.
                                   rayleigh_scale_height_metres
                             <<"},\"exposure_ev\":"<<world_exposure_ev
                             <<",\"camera_feet\":["
                             <<world_controller.state().feet.x<<','
                             <<world_controller.state().feet.y<<','
                             <<world_controller.state().feet.z
                             <<"],\"camera_yaw_degrees\":"
                             <<world_controller.state().yaw*180.0/
                                   std::numbers::pi
                             <<",\"camera_pitch_degrees\":"
                             <<world_controller.state().pitch*180.0/
                                   std::numbers::pi
                             <<",\"sun_azimuth_degrees\":"
                             <<world_sun_azimuth*180.0F/
                                   static_cast<float>(std::numbers::pi)
                             <<",\"sun_elevation_degrees\":"
                             <<world_sun_elevation*180.0F/
                                   static_cast<float>(std::numbers::pi)
                             <<",\"sun_screen_visible\":"
                             <<(projected_sun.visible?"true":"false")
                             <<",\"sun_pixel\":["<<sun_pixel_x<<','
                             <<sun_pixel_y<<']'
                             <<",\"sun_centre_geometry_occluded\":"
                             <<(sun_centre_geometry_occluded?"true":"false")
                             <<",\"analysis\":{\"minimum\":["
                             <<static_cast<unsigned>(analysis.minimum[0])<<','
                             <<static_cast<unsigned>(analysis.minimum[1])<<','
                             <<static_cast<unsigned>(analysis.minimum[2])
                             <<"],\"maximum\":["
                             <<static_cast<unsigned>(analysis.maximum[0])<<','
                             <<static_cast<unsigned>(analysis.maximum[1])<<','
                             <<static_cast<unsigned>(analysis.maximum[2])
                             <<"],\"luminance_mean\":"
                             <<analysis.luminance_mean
                             <<",\"luminance_standard_deviation\":"
                             <<analysis.luminance_standard_deviation
                             <<",\"black_fraction\":"
                             <<analysis.black_fraction
                             <<",\"clipped_fraction\":"
                             <<analysis.clipped_fraction
                             <<",\"geometry_fraction\":"
                             <<static_cast<double>(std::ranges::count_if(
                                   depth_mask,[](std::uint8_t value){
                                     return value!=0U;}))/
                                   static_cast<double>(depth_mask.size())
                             <<"},\"outer_limb\":{\"sampled_pixels\":"
                             <<outer_limb_analysis.sampled_pixels
                             <<",\"mean\":["
                             <<outer_limb_analysis.mean[0]<<','
                             <<outer_limb_analysis.mean[1]<<','
                             <<outer_limb_analysis.mean[2]
                             <<"],\"luminance_mean\":"
                             <<outer_limb_analysis.luminance_mean
                             <<",\"black_fraction\":"
                             <<outer_limb_analysis.black_fraction
                             <<",\"blue_fraction\":"
                             <<(outer_limb_analysis.sampled_pixels==0U?0.0:
                                 static_cast<double>(blue_outer_limb_pixels)/
                                 static_cast<double>(
                                     outer_limb_analysis.sampled_pixels))
                             <<'}';
                    const auto& capture_timing=g_SceneRenderer.gpu_timings();
                    std::cout<<",\"gpu_timings\":{\"valid\":"
                             <<(capture_timing.valid?"true":"false")
                             <<",\"depth_reduction_ms\":"
                             <<capture_timing.depth_reduction_milliseconds
                             <<",\"screen_integration_ms\":"
                             <<capture_timing.screen_integration_milliseconds
                             <<",\"temporal_reconstruction_ms\":"
                             <<capture_timing.temporal_reconstruction_milliseconds
                             <<",\"composite_ms\":"
                             <<capture_timing.composite_milliseconds<<'}';
                    const auto& shadow_status=
                        g_SceneRenderer.atmosphere_shadow_map_status();
                    std::cout<<",\"shadow_diagnostics\":{\"hierarchy_complete\":"
                             <<(shadow_status.hierarchy_complete?"true":"false")
                             <<",\"epipolar_visited_nodes\":"
                             <<shadow_status.epipolar_visited_nodes
                             <<",\"epipolar_emitted_intervals\":"
                             <<shadow_status.epipolar_emitted_intervals
                             <<",\"epipolar_fallbacks\":"
                             <<shadow_status.epipolar_fallbacks
                             <<",\"epipolar_overflows\":"
                             <<shadow_status.epipolar_overflows
                             <<",\"epipolar_hierarchy_refreshes\":"
                             <<shadow_status.epipolar_hierarchy_refreshes
                             <<",\"comparison_bias_world\":"
                             <<shadow_status.comparison_bias_world
                             <<",\"raster_bias_constant\":"
                             <<shadow_status.raster_bias_constant
                             <<",\"raster_bias_slope\":"
                             <<shadow_status.raster_bias_slope<<'}';
                    const auto& revisions=
                        g_SceneRenderer.latest_atmosphere_lookup_revisions();
                    if(revisions){
                        std::cout<<",\"lookup_revisions\":{\"optical\":"
                                 <<revisions->optical.value
                                 <<",\"scattering\":"
                                 <<revisions->scattering.value
                                 <<",\"sun\":"<<revisions->sun.value
                                 <<",\"camera_position\":"
                                 <<revisions->camera_position.value
                                 <<",\"sky_position\":"
                                 <<revisions->sky_position.value
                                 <<",\"camera_orientation\":"
                                 <<revisions->camera_orientation.value
                                 <<",\"shadow\":"<<revisions->shadow.value
                                 <<",\"render_origin\":"
                                 <<revisions->render_origin.value<<'}';
                    }
                    std::cout<<"}\n";
                }
                world_gpu_atmosphere_capture_path.clear();
            }
            if(world_gpu_automation_requested&&
               !world_gpu_atmosphere_benchmark&&
               !world_gpu_atmosphere_probe&&
               !world_gpu_shadow_projection_probe&&
               world_gpu_atmosphere_capture_path.empty()){
                glfwSetWindowShouldClose(window,GLFW_TRUE);
            }
        }
    }

    // Cleanup
    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);
    g_SceneRenderer.shutdown();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();

    glfwDestroyWindow(window);
    glfwTerminate();

    return world_process_exit_code;
}
