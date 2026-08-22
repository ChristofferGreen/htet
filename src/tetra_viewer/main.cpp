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
    tetra::Camera camera{};
    float sphere_centre[3]{0.5f, 0.5f, 0.5f};
    float sphere_radius = static_cast<float>(sphere.radius);
    float camera_distance = 2.5f;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
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
    bool orbit_dragging = false;
    double previous_cursor_x = 0.0;
    double previous_cursor_y = 0.0;
    if (argc > 1 && strcmp(argv[1], "--sphere-far") == 0) camera_distance = 12.0f;
    if (argc > 1 && strcmp(argv[1], "--sphere-fine") == 0) { pixel_threshold = 40.0f; maximum_depth = 3; }
    if (argc > 1 && strcmp(argv[1], "--sphere-offcentre") == 0) {
        sphere_centre[0] = 0.43f; sphere_centre[1] = 0.57f; sphere_centre[2] = 0.46f;
        sphere.centre = {sphere_centre[0], sphere_centre[1], sphere_centre[2]}; sphere.radius = sphere_radius = 0.27f;
    }
    if (deterministic_visual_check) {
        if(!whole_cell_check&&!whole_cell_cutaway_check)
            subdivision_method_index = static_cast<std::size_t>(std::distance(
                tetra::subdivision_methods.begin(),
                std::find(tetra::subdivision_methods.begin(),tetra::subdivision_methods.end(),
                          tetra::SubdivisionMethod::bcc_red_green)));
        camera_distance = 0.70F;
        camera_yaw = -0.28F;
        camera_pitch = 0.48F;
        if(whole_cell_check||whole_cell_cutaway_check)camera_distance=1.10F;
    }
    const auto update_orbit_camera = [&] {
        const double horizontal = std::cos(camera_pitch);
        camera.position = {sphere.centre.x + camera_distance * horizontal * std::sin(camera_yaw),
                           sphere.centre.y + camera_distance * std::sin(camera_pitch),
                           sphere.centre.z + camera_distance * horizontal * std::cos(camera_yaw)};
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
    update_orbit_camera();
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
        ImGui::SeparatorText("Sphere and refinement");
        ImGui::TextDisabled("Sphere centre");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat3("##Sphere centre", sphere_centre, 0.05f, 0.95f)) {
            sphere.centre = {sphere_centre[0], sphere_centre[1], sphere_centre[2]};
            ++sphere_revision;
            has_adaptive_result = false;
        }
        ImGui::TextDisabled("Sphere radius");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##Sphere radius", &sphere_radius, 0.05f, 0.48f)) {
            sphere.radius = sphere_radius;
            ++sphere_revision;
            has_adaptive_result = false;
        }
        ImGui::TextWrapped("Drag the viewport to orbit; scroll to zoom.");
        ImGui::TextDisabled("LOD origin: %.2f, %.2f, %.2f", camera.position.x, camera.position.y, camera.position.z);
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
            last_adaptive_result = refine_to_current_surface();
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
        const bool orbit_allowed = !deterministic_visual_check && !controls_hovered;
        if (orbit_allowed) {
            double cursor_x = 0.0, cursor_y = 0.0;
            glfwGetCursorPos(window, &cursor_x, &cursor_y);
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (orbit_dragging) {
                    camera_yaw += static_cast<float>((cursor_x - previous_cursor_x) * 0.008);
                    camera_pitch = std::clamp(camera_pitch + static_cast<float>((cursor_y - previous_cursor_y) * 0.008), -1.45f, 1.45f);
                }
                orbit_dragging = true;
                previous_cursor_x = cursor_x;
                previous_cursor_y = cursor_y;
            } else {
                orbit_dragging = false;
            }
            const auto& input = ImGui::GetIO();
            // macOS commonly converts Shift+vertical-wheel into a horizontal
            // wheel event. Treat that shifted delta as precision dolly input.
            const float wheel = input.MouseWheel != 0.0f
                ? input.MouseWheel
                : (input.KeyShift ? input.MouseWheelH : 0.0f);
            if (wheel != 0.0f) {
                // Multiplicative dolly alone can never reach the orbit target.
                // Retain proportional movement at normal distances, then use
                // a small linear step close in so scrolling can reach exactly
                // zero and can move away again on the next reverse scroll.
                const float precision_scale = input.KeyShift ? 0.15f : 1.0f;
                const float zoom_step = std::max(camera_distance * 0.22f, 0.05f) * precision_scale;
                camera_distance = std::clamp(camera_distance - wheel * zoom_step, 0.0f, 20.0f);
            }
        } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE) {
            orbit_dragging = false;
        }
        update_orbit_camera();
        // Derive the view direction from the orbit angles rather than
        // target-position subtraction. At distance zero both positions are
        // equal, but the camera must retain a well-defined orientation.
        const double horizontal = std::cos(camera_pitch);
        const tetra::Vec3 f{-horizontal * std::sin(camera_yaw),
                            -std::sin(camera_pitch),
                            -horizontal * std::cos(camera_yaw)};
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
        const tetra::Vec3 camera_position{camera.position};
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
            g_SceneRenderer.upload(prepared_scene.triangle_vertices,
                                   prepared_scene.hierarchy_line_vertices,
                                   prepared_scene.surface_line_vertices);
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
