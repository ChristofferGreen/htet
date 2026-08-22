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
#include "scene_renderer.hpp"
#include "tetra_viewer/viewer_script.hpp"
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
static std::array<float, 24> g_CameraData{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};

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
            device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
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
    const VkExtent2D extent{static_cast<std::uint32_t>(wd->Width), static_cast<std::uint32_t>(wd->Height)};
    g_SceneRenderer.record(fd->CommandBuffer, fd->BackbufferView, wd->FrameIndex, extent, g_CameraData.data());
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
int main(int argc, char** argv)
{
    // The headless path intentionally returns before any platform Vulkan
    // loading, GLFW initialization, or window creation.
    if (argc >= 2 && strcmp(argv[1], "--script-help") == 0) {
        if (argc != 2) {
            fprintf(stderr, "--script-help does not accept arguments\n");
            return 2;
        }
        tetra_viewer::print_script_help(std::cout);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--script") == 0) {
        if (argc != 3) {
            fprintf(stderr, "usage: tetra_viewer --script \"command[,command...]\"\n");
            return 2;
        }
        return tetra_viewer::run_script(argv[2], std::cout, std::cerr);
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
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Tetrahedral refinement", nullptr, nullptr);
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
    g_SceneRenderer.recreate({static_cast<std::uint32_t>(wd->Width), static_cast<std::uint32_t>(wd->Height)}, wd->ImageCount);

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
    const bool deterministic_visual_check = wireframe_check || tetweave_cutaway_check ||
        selected_atlas_check || whole_cell_check || whole_cell_cutaway_check;
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
    tetra_viewer::VolumeConnectionMethod volume_connection_method =
        (tetweave_cutaway_check || selected_atlas_check)
            ? tetra_viewer::VolumeConnectionMethod::quality_stencils
            : tetra_viewer::default_volume_connection_method;
    if(!deterministic_visual_check)
        volume_connection_method=tetra_viewer::default_volume_connection_for_shape(
            tetra_viewer::default_implicit_shape);
    tetra_viewer::StencilConstruction stencil_construction =
        (tetweave_cutaway_check || selected_atlas_check)
            ? tetra_viewer::StencilConstruction::selected
            : tetra_viewer::StencilConstruction::fixed;
    tetra_viewer::StencilSelectionObjective stencil_selection_objective =
        tetra_viewer::StencilSelectionObjective::balanced;
    tetra_viewer::ShadingModel shading_model = selected_atlas_check
        ? tetra_viewer::ShadingModel::dihedral_angle
        : tetra_viewer::ShadingModel::studio_flat;
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
    bool show_faces = true;
    bool show_hierarchy_edges = false;
    bool show_surface_edges = true;
    bool show_volume_edges = true;
    bool show_volume_faces = true;
    bool x_cutaway=deterministic_visual_check
        ?wireframe_check||tetweave_cutaway_check||whole_cell_cutaway_check
        :true;
    float x_cut_position = 0.5F;
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
    std::uint64_t sphere_revision = 0;
    bool upload_dirty = true;
    enum class CameraDragMode { none, orbit, pan };
    CameraDragMode camera_drag_mode=CameraDragMode::none;
    bool lod_camera_selected=false;
    tetra_viewer::CameraGizmoMode camera_gizmo_mode=
        tetra_viewer::CameraGizmoMode::select;
    tetra_viewer::CameraGizmoAxis active_gizmo_axis=
        tetra_viewer::CameraGizmoAxis::none;
    bool gizmo_dragging=false;
    bool lod_reconcile_pending=false;
    bool previous_left_pressed=false;
    double previous_rotation_angle{};
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
        return tetra::refine_to_sphere(
            mesh, sphere, camera, pixel_threshold, static_cast<unsigned int>(maximum_depth));
    };
    const auto reconcile_to_current_surface=[&] {
        mesh.reset_active_hierarchy();
        return refine_to_current_surface();
    };
    update_orbit_camera();
    lod_camera_pose.apply(camera);
    if (sphere_mode) {
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

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        // Resize swap chain?
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
            g_SceneRenderer.recreate({static_cast<std::uint32_t>(g_MainWindowData.Width), static_cast<std::uint32_t>(g_MainWindowData.Height)}, g_MainWindowData.ImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

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

        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(300.0f, 0.0f),
            ImVec2(340.0f, std::max(240.0f, ImGui::GetIO().DisplaySize.y - 32.0f)));
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
        ImGui::Checkbox("X cutaway", &x_cutaway);
        if (x_cutaway) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##X cut position", &x_cut_position, 0.0F, 1.0F, "x <= %.3f");
            ImGui::TextWrapped("Tetrahedra touching the right side are hidden; retained cells stay whole and do not protrude through the plane. Interior cells are blue and the conforming boundary layer is orange.");
        }
        ImGui::SeparatorText("Implicit shape and refinement");
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
        ImGui::TextWrapped("Click the camera, then use Q/W/E for select, move, or rotate. Releasing a gizmo rebuilds the active hierarchy to match the new view.");
        ImGui::TextDisabled("Origin: %.2f, %.2f, %.2f   Direction: %.2f, %.2f, %.2f",
                            camera.position.x,camera.position.y,camera.position.z,
                            camera.forward.x,camera.forward.y,camera.forward.z);
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
        if (ImGui::SliderFloat("##Pixel threshold", &pixel_threshold, 4.0f, 240.0f, "%.0f px"))
            has_adaptive_result = false;
        ImGui::TextDisabled("Maximum depth");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##Maximum depth", &maximum_depth, 1, 32)) {
            has_adaptive_result = false;
        }
        update_orbit_camera();
        const double viewport_height = static_cast<double>(std::max(1.0F, ImGui::GetIO().DisplaySize.y));
        camera.viewport_height_pixels = viewport_height;
        camera.aspect_ratio=static_cast<double>(std::max(1.0F,ImGui::GetIO().DisplaySize.x))/viewport_height;
        if (ImGui::BeginTable("mesh actions", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button("Reset", ImVec2(-FLT_MIN, 0.0f))) {
            mesh = tetra::TetMesh::make_unit_cube(tetra::subdivision_methods[subdivision_method_index]);
            refined = false;
            has_adaptive_result = false;
            mesh_valid = true;
            mesh_validation_current = true;
            last_validation_milliseconds = -1.0;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Refine once", ImVec2(-FLT_MIN, 0.0f))) {
            const auto start = std::chrono::steady_clock::now();
            mesh.refine_all_binary();
            last_refine_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            refined = true;
            mesh_validation_current = false;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Refine to target", ImVec2(-FLT_MIN, 0.0f))) {
            const auto start = std::chrono::steady_clock::now();
            last_adaptive_result = reconcile_to_current_surface();
            last_refine_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            has_adaptive_result = true;
            refined = true;
            mesh_validation_current = false;
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
        const auto preparation_start = std::chrono::steady_clock::now();
        if (scene_cache.update_scene(mesh, sphere, sphere_revision, surface_method,
                                     tetra_viewer::material_rules[material_rule_index],
                                     show_faces, show_hierarchy_edges, show_surface_edges,
                                     depth_colours, x_cutaway && show_volume_edges,
                                     x_cutaway && show_volume_faces, x_cut_position,
                                     volume_connection_method,stencil_construction,
                                     stencil_selection_objective)) {
            last_scene_preparation_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - preparation_start).count();
            upload_dirty = true;
        }
        scene_cache.update_projection(mesh, camera, pixel_threshold);
        const auto& prepared_scene = scene_cache.scene();
        const auto& projection_statistics = scene_cache.projection();
        if (ImGui::CollapsingHeader("Statistics")) {
        ImGui::Text("Active leaves: %zu", mesh.active_leaves().size());
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
        }
        const bool controls_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::End();
        // Let Dear ImGui reserve pointer input over the floating controls;
        // otherwise use raw GLFW drag state for dependable orbiting.
        // The wireframe regression view must remain deterministic for Vulkan
        // screenshot validation; do not let residual trackpad-wheel inertia
        // move its camera immediately after launch.
        const bool camera_input_allowed=!deterministic_visual_check&&!controls_hovered;
        bool lod_camera_moved=false;
        const auto& input=ImGui::GetIO();
        if(!input.WantCaptureKeyboard){
            auto keyboard_mode=camera_gizmo_mode;
            if(glfwGetKey(window,GLFW_KEY_Q)==GLFW_PRESS)
                keyboard_mode=tetra_viewer::CameraGizmoMode::select;
            else if(glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS)
                keyboard_mode=tetra_viewer::CameraGizmoMode::translate;
            else if(glfwGetKey(window,GLFW_KEY_E)==GLFW_PRESS)
                keyboard_mode=tetra_viewer::CameraGizmoMode::rotate;
            if(keyboard_mode!=camera_gizmo_mode){camera_gizmo_mode=keyboard_mode;upload_dirty=true;}
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
            const auto camera_pick_distance=[&](){
                const auto normalize=[](tetra::Vec3 value){
                    const double length=std::sqrt(value.x*value.x+value.y*value.y+
                                                  value.z*value.z);
                    return length>1.0e-15?value/length:tetra::Vec3{};
                };
                const auto cross=[](tetra::Vec3 first,tetra::Vec3 second){
                    return tetra::Vec3{first.y*second.z-first.z*second.y,
                                       first.z*second.x-first.x*second.z,
                                       first.x*second.y-first.y*second.x};
                };
                const auto camera_right=normalize(cross(
                    lod_camera_pose.forward,lod_camera_pose.up));
                const auto camera_up=normalize(cross(
                    camera_right,lod_camera_pose.forward));
                constexpr double scale=0.16;
                const auto tip=lod_camera_pose.position+lod_camera_pose.forward*scale;
                const std::array<tetra::Vec3,4> corners{{
                    tip+camera_right*(scale*0.55)+camera_up*(scale*0.40),
                    tip-camera_right*(scale*0.55)+camera_up*(scale*0.40),
                    tip-camera_right*(scale*0.55)-camera_up*(scale*0.40),
                    tip+camera_right*(scale*0.55)-camera_up*(scale*0.40)}};
                double distance=std::hypot(cursor_x-camera_screen.x,
                                           cursor_y-camera_screen.y);
                for(const auto corner:corners)
                    distance=std::min(distance,segment_distance(cursor_x,cursor_y,
                        camera_screen,project(corner)));
                for(std::size_t index=0;index<corners.size();++index)
                    distance=std::min(distance,segment_distance(cursor_x,cursor_y,
                        project(corners[index]),project(corners[(index+1)%corners.size()])));
                return distance;
            };
            constexpr double gizmo_scale=0.28;
            const auto pick_axis=[&](){
                auto best=tetra_viewer::CameraGizmoAxis::none;
                double best_distance=9.0;
                for(const auto axis:{tetra_viewer::CameraGizmoAxis::x,
                                     tetra_viewer::CameraGizmoAxis::y,
                                     tetra_viewer::CameraGizmoAxis::z}){
                    double distance=std::numeric_limits<double>::infinity();
                    if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::translate){
                        distance=segment_distance(cursor_x,cursor_y,camera_screen,
                            project(lod_camera_pose.position+
                                tetra_viewer::LodCameraPose::axis(axis)*gizmo_scale));
                    }else if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::rotate){
                        tetra::Vec3 first_basis{},second_basis{};
                        if(axis==tetra_viewer::CameraGizmoAxis::x){first_basis={0,1,0};second_basis={0,0,1};}
                        if(axis==tetra_viewer::CameraGizmoAxis::y){first_basis={1,0,0};second_basis={0,0,1};}
                        if(axis==tetra_viewer::CameraGizmoAxis::z){first_basis={1,0,0};second_basis={0,1,0};}
                        constexpr std::size_t segments=48;
                        for(std::size_t index=0;index<segments;++index){
                            const double first_angle=2.0*std::acos(-1.0)*index/segments;
                            const double second_angle=2.0*std::acos(-1.0)*(index+1)/segments;
                            const auto ring_point=[&](double angle){return lod_camera_pose.position+
                                (first_basis*std::cos(angle)+second_basis*std::sin(angle))*gizmo_scale;};
                            distance=std::min(distance,segment_distance(cursor_x,cursor_y,
                                project(ring_point(first_angle)),project(ring_point(second_angle))));
                        }
                    }
                    if(distance<best_distance){best_distance=distance;best=axis;}
                }
                return best;
            };

            if(left_started){
                previous_cursor_x=cursor_x;
                previous_cursor_y=cursor_y;
                if(alt_pressed){
                    camera_drag_mode=shift_pressed?CameraDragMode::pan:CameraDragMode::orbit;
                }else{
                    const auto picked=lod_camera_selected?pick_axis():
                        tetra_viewer::CameraGizmoAxis::none;
                    if(picked!=tetra_viewer::CameraGizmoAxis::none&&
                       camera_gizmo_mode!=tetra_viewer::CameraGizmoMode::select){
                        active_gizmo_axis=picked;
                        gizmo_dragging=true;
                        previous_rotation_angle=std::atan2(
                            cursor_y-camera_screen.y,cursor_x-camera_screen.x);
                    }else if(camera_screen.visible&&camera_pick_distance()<=10.0){
                        lod_camera_selected=true;
                        upload_dirty=true;
                    }else{
                        lod_camera_selected=false;
                        active_gizmo_axis=tetra_viewer::CameraGizmoAxis::none;
                        // A primary-button drag on empty space orbits the
                        // editor view. Shift-drag pans. A stationary click
                        // still behaves as ordinary selection/deselection.
                        camera_drag_mode=shift_pressed?CameraDragMode::pan:
                            CameraDragMode::orbit;
                        upload_dirty=true;
                    }
                }
            }
            if(left_pressed){
                if(gizmo_dragging&&
                   active_gizmo_axis!=tetra_viewer::CameraGizmoAxis::none){
                    if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::translate){
                        const auto axis_end=project(lod_camera_pose.position+
                            tetra_viewer::LodCameraPose::axis(active_gizmo_axis)*gizmo_scale);
                        const double dx=axis_end.x-camera_screen.x;
                        const double dy=axis_end.y-camera_screen.y;
                        const double length_squared=dx*dx+dy*dy;
                        if(length_squared>1.0e-8){
                            const double pixels=(cursor_x-previous_cursor_x)*dx+
                                (cursor_y-previous_cursor_y)*dy;
                            lod_camera_pose.translate(active_gizmo_axis,
                                pixels/length_squared*gizmo_scale*precision);
                            lod_camera_moved=pixels!=0.0;
                        }
                    }else if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::rotate){
                        const double angle=std::atan2(
                            cursor_y-camera_screen.y,cursor_x-camera_screen.x);
                        double delta=angle-previous_rotation_angle;
                        if(delta>std::acos(-1.0))delta-=2.0*std::acos(-1.0);
                        if(delta<-std::acos(-1.0))delta+=2.0*std::acos(-1.0);
                        lod_camera_pose.rotate(active_gizmo_axis,delta*precision);
                        previous_rotation_angle=angle;
                        lod_camera_moved=delta!=0.0;
                    }
                }else if(camera_drag_mode!=CameraDragMode::none){
                    const double delta_x=cursor_x-previous_cursor_x;
                    const double delta_y=cursor_y-previous_cursor_y;
                    if(camera_drag_mode==CameraDragMode::orbit)
                        orbit_camera.orbit(delta_x,delta_y,precision);
                    else orbit_camera.pan(delta_x,delta_y,viewport_height,
                                          camera.vertical_fov_radians,precision);
                }
            }else{
                gizmo_dragging=false;
                active_gizmo_axis=tetra_viewer::CameraGizmoAxis::none;
                camera_drag_mode=CameraDragMode::none;
            }
            if(left_pressed){previous_cursor_x=cursor_x;previous_cursor_y=cursor_y;}
            previous_left_pressed=left_pressed;
            const float wheel=input.MouseWheel!=0.0f?input.MouseWheel:
                (shift_pressed?input.MouseWheelH:0.0f);
            if(wheel!=0.0f)orbit_camera.dolly(wheel,precision);
        }else{
            camera_drag_mode=CameraDragMode::none;
            previous_left_pressed=false;
        }
        if(lod_camera_moved){
            lod_camera_pose.apply(camera);
            has_adaptive_result=false;
            lod_reconcile_pending=true;
            upload_dirty=true;
        }
        // Reconcile once on release rather than rebuilding for every pointer
        // sample. The packed hierarchy and midpoint vertices remain resident;
        // only its active cut is collapsed and refined for the new camera.
        if(lod_reconcile_pending&&!gizmo_dragging&&!previous_left_pressed){
            const auto start=std::chrono::steady_clock::now();
            last_adaptive_result=reconcile_to_current_surface();
            last_refine_milliseconds=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-start).count();
            has_adaptive_result=true;
            refined=true;
            mesh_validation_current=false;
            upload_dirty=true;
            lod_reconcile_pending=false;
        }
        update_orbit_camera();
        // Derive the view direction from the orbit angles rather than
        // target-position subtraction. At distance zero both positions are
        // equal, but the camera must retain a well-defined orientation.
        const tetra::Vec3 f=orbit_camera.forward();
        const tetra::Vec3 right_seed{f.z, 0.0, -f.x};
        const double right_length = std::sqrt(right_seed.x * right_seed.x + right_seed.z * right_seed.z);
        const tetra::Vec3 right{right_seed.x / right_length, 0.0, right_seed.z / right_length};
        const tetra::Vec3 up{right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x};
        const float tangent = static_cast<float>(std::tan(camera.vertical_fov_radians * 0.5));
        const float aspect = ImGui::GetIO().DisplaySize.x / std::max(1.0F, ImGui::GetIO().DisplaySize.y);
        const float scale_x = 1.0F / (tangent * aspect);
        const float scale_y = 1.0F / tangent;
        constexpr float near_plane = 0.001F, far_plane = 100.0F;
        const float depth_scale = far_plane / (far_plane - near_plane);
        const float depth_bias = -far_plane * near_plane / (far_plane - near_plane);
        const auto dot = [](const tetra::Vec3& first, const tetra::Vec3& second) { return first.x*second.x + first.y*second.y + first.z*second.z; };
        const tetra::Vec3 camera_position{view_camera_position};
        const float tx = static_cast<float>(-dot(right, camera_position) * scale_x);
        const float ty = static_cast<float>(-dot(up, camera_position) * scale_y);
        const float tz = static_cast<float>(-dot(f, camera_position) * depth_scale + depth_bias);
        const float tw = static_cast<float>(-dot(f, camera_position));
        g_CameraData = {
            static_cast<float>(right.x * scale_x), static_cast<float>(up.x * scale_y), static_cast<float>(f.x * depth_scale), static_cast<float>(f.x),
            static_cast<float>(right.y * scale_x), static_cast<float>(up.y * scale_y), static_cast<float>(f.y * depth_scale), static_cast<float>(f.y),
            static_cast<float>(right.z * scale_x), static_cast<float>(up.z * scale_y), static_cast<float>(f.z * depth_scale), static_cast<float>(f.z),
            tx, ty, tz, tw,
            static_cast<float>(-f.x), static_cast<float>(-f.y), static_cast<float>(-f.z), show_volume_edges ? 1.0F : 0.0F,
            static_cast<float>(shading_model), show_surface_edges ? 1.0F : 0.0F,
            show_faces ? 1.0F : 0.0F, x_cutaway ? x_cut_position : 2.0F};
        if (upload_dirty) {
            // Editor overlays deliberately do not include mesh surface-edge
            // segments. The mesh wireframe has its own depth-tested native
            // line pass; mixing it here would recreate the see-through bug.
            std::vector<tetra_viewer::SceneVertex> overlay_lines;
            const auto add_overlay_line=[&](tetra::Vec3 first,tetra::Vec3 second,
                                            std::array<float,3> colour){
                const auto vertex=[&](tetra::Vec3 point){
                    tetra_viewer::SceneVertex result;
                    result.position[0]=static_cast<float>(point.x);
                    result.position[1]=static_cast<float>(point.y);
                    result.position[2]=static_cast<float>(point.z);
                    result.colour[0]=colour[0];result.colour[1]=colour[1];result.colour[2]=colour[2];
                    return result;
                };
                overlay_lines.push_back(vertex(first));
                overlay_lines.push_back(vertex(second));
            };
            if(!deterministic_visual_check){
            const auto normalize=[](tetra::Vec3 value){
                const double length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
                return length>1.0e-15?value/length:tetra::Vec3{};
            };
            const auto cross=[](tetra::Vec3 first,tetra::Vec3 second){
                return tetra::Vec3{first.y*second.z-first.z*second.y,
                                   first.z*second.x-first.x*second.z,
                                   first.x*second.y-first.y*second.x};
            };
            const auto camera_right=normalize(cross(lod_camera_pose.forward,lod_camera_pose.up));
            const auto camera_up=normalize(cross(camera_right,lod_camera_pose.forward));
            constexpr double camera_scale=0.16;
            const auto camera_tip=lod_camera_pose.position+lod_camera_pose.forward*camera_scale;
            const std::array<tetra::Vec3,4> corners{{
                camera_tip+camera_right*(camera_scale*0.55)+camera_up*(camera_scale*0.40),
                camera_tip-camera_right*(camera_scale*0.55)+camera_up*(camera_scale*0.40),
                camera_tip-camera_right*(camera_scale*0.55)-camera_up*(camera_scale*0.40),
                camera_tip+camera_right*(camera_scale*0.55)-camera_up*(camera_scale*0.40)}};
            const std::array<float,3> camera_colour=lod_camera_selected?
                std::array<float,3>{1.0F,0.82F,0.18F}:std::array<float,3>{0.86F,0.88F,0.94F};
            for(const auto corner:corners)add_overlay_line(
                lod_camera_pose.position,corner,camera_colour);
            for(std::size_t index=0;index<corners.size();++index)add_overlay_line(
                corners[index],corners[(index+1)%corners.size()],camera_colour);
            add_overlay_line(camera_tip+camera_up*(camera_scale*0.40),
                             camera_tip+camera_up*(camera_scale*0.72),camera_colour);

            if(lod_camera_selected&&camera_gizmo_mode!=tetra_viewer::CameraGizmoMode::select){
                constexpr double gizmo_scale=0.28;
                constexpr std::array axes{
                    tetra_viewer::CameraGizmoAxis::x,
                    tetra_viewer::CameraGizmoAxis::y,
                    tetra_viewer::CameraGizmoAxis::z};
                constexpr std::array<std::array<float,3>,3> colours{{
                    {{0.95F,0.18F,0.16F}},{{0.25F,0.90F,0.30F}},{{0.20F,0.48F,1.0F}}}};
                for(std::size_t index=0;index<axes.size();++index){
                    const auto axis=axes[index];
                    if(camera_gizmo_mode==tetra_viewer::CameraGizmoMode::translate){
                        add_overlay_line(lod_camera_pose.position,lod_camera_pose.position+
                            tetra_viewer::LodCameraPose::axis(axis)*gizmo_scale,colours[index]);
                    }else{
                        tetra::Vec3 first_basis{},second_basis{};
                        if(axis==tetra_viewer::CameraGizmoAxis::x){first_basis={0,1,0};second_basis={0,0,1};}
                        if(axis==tetra_viewer::CameraGizmoAxis::y){first_basis={1,0,0};second_basis={0,0,1};}
                        if(axis==tetra_viewer::CameraGizmoAxis::z){first_basis={1,0,0};second_basis={0,1,0};}
                        constexpr std::size_t segments=48;
                        for(std::size_t segment=0;segment<segments;++segment){
                            const double first_angle=2.0*std::acos(-1.0)*segment/segments;
                            const double second_angle=2.0*std::acos(-1.0)*(segment+1)/segments;
                            const auto ring_point=[&](double angle){return lod_camera_pose.position+
                                (first_basis*std::cos(angle)+second_basis*std::sin(angle))*gizmo_scale;};
                            add_overlay_line(ring_point(first_angle),ring_point(second_angle),colours[index]);
                        }
                    }
                }
            }
            }
            g_SceneRenderer.upload(prepared_scene.triangle_vertices,
                                   prepared_scene.hierarchy_line_vertices,
                                   overlay_lines);
            upload_dirty = false;
        }

        // Rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
        // Avoid spinning the event loop when present returns immediately.
        ImGui_ImplGlfw_Sleep(16);
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

    return 0;
}
