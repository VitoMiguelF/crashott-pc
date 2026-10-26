// Android entry point (docs/internals/android_port.md). SDL3's Java glue (SDLActivity) loads
// libSDL3.so and libmain.so and calls SDL_main on its own native thread, "SDLThread". That thread
// keeps SDL's window and events (gpu/window_sdl.cpp hands its loop over with RunOnMainThread);
// the game (main.cpp's CrashMain) runs on a thread of its own with a 64 MB stack, as guest
// threads have.
//
// Before the game, a Vulkan probe opens a window and a surface, logs what the GPU offers that the
// renderer needs (geometry shaders, BC textures, push descriptors...) and clears the screen for a
// moment, so a first install shows whether the device can draw at all.
//
// Folders: the app's external files folder (/storage/emulated/0/Android/data/<package>/files)
// stands for the PC's exe folder: game/, userdata/, mods/, data/, languages/. args.txt there (one
// argument per line) gives command-line arguments (--skip-intros, --boot ...).

#include "android_main.h"
#include "platform.h"
#include "../diagnostics.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <android/log.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

int CrashMain(int argc, char** argv);

namespace
{
    std::mutex g_jobsMutex;
    std::condition_variable g_jobsChanged;
    std::deque<std::function<void()>> g_jobs;
    bool g_gameFinished = false;
    int g_gameExitCode = 0;

    std::string Property(const char* name)
    {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get(name, value);
        return value;
    }

    // ---- Vulkan probe --------------------------------------------------------------------------
    struct Vk
    {
        PFN_vkGetInstanceProcAddr getInstanceProcAddr{};
        VkInstance instance{};
        template <typename T>
        T Get(const char* name) const
        {
            return reinterpret_cast<T>(getInstanceProcAddr(instance, name));
        }
    };

    bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
    {
        for (const VkExtensionProperties& e : extensions)
        {
            if (strcmp(e.extensionName, name) == 0)
                return true;
        }
        return false;
    }

    // Clears the swapchain to the project's orange for `seconds`, pumping events.
    void ClearScreen(const Vk& vk, VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface, double seconds)
    {
        auto createDevice = vk.Get<PFN_vkCreateDevice>("vkCreateDevice");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = extensions;
        VkDevice device{};
        if (createDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
        {
            Log("[android] vkCreateDevice failed");
            return;
        }
        auto getDeviceProcAddr = vk.Get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        auto D = [&](const char* name) { return getDeviceProcAddr(device, name); };
        auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(D("vkGetDeviceQueue"));
        auto createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(D("vkCreateSwapchainKHR"));
        auto getImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(D("vkGetSwapchainImagesKHR"));
        auto acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(D("vkAcquireNextImageKHR"));
        auto present = reinterpret_cast<PFN_vkQueuePresentKHR>(D("vkQueuePresentKHR"));
        auto createPool = reinterpret_cast<PFN_vkCreateCommandPool>(D("vkCreateCommandPool"));
        auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(D("vkAllocateCommandBuffers"));
        auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(D("vkBeginCommandBuffer"));
        auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(D("vkEndCommandBuffer"));
        auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(D("vkCmdPipelineBarrier"));
        auto clear = reinterpret_cast<PFN_vkCmdClearColorImage>(D("vkCmdClearColorImage"));
        auto submit = reinterpret_cast<PFN_vkQueueSubmit>(D("vkQueueSubmit"));
        auto createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(D("vkCreateSemaphore"));
        auto createFence = reinterpret_cast<PFN_vkCreateFence>(D("vkCreateFence"));
        auto waitFences = reinterpret_cast<PFN_vkWaitForFences>(D("vkWaitForFences"));
        auto resetFences = reinterpret_cast<PFN_vkResetFences>(D("vkResetFences"));
        auto resetBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(D("vkResetCommandBuffer"));
        auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(D("vkDeviceWaitIdle"));
        auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(D("vkDestroyDevice"));
        auto destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(D("vkDestroySwapchainKHR"));

        auto caps = vk.Get<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        auto formats = vk.Get<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
        VkSurfaceCapabilitiesKHR capabilities{};
        caps(physical, surface, &capabilities);
        uint32_t formatCount = 0;
        formats(physical, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
        formats(physical, surface, &formatCount, surfaceFormats.data());
        if (surfaceFormats.empty() || !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        {
            Log("[android] the surface can't be cleared by a transfer (usage 0x%X): no clear", capabilities.supportedUsageFlags);
            destroyDevice(device, nullptr);
            return;
        }

        VkSwapchainCreateInfoKHR swapInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        swapInfo.surface = surface;
        swapInfo.minImageCount = std::max(capabilities.minImageCount, 2u);
        if (capabilities.maxImageCount && swapInfo.minImageCount > capabilities.maxImageCount)
            swapInfo.minImageCount = capabilities.maxImageCount;
        swapInfo.imageFormat = surfaceFormats[0].format;
        swapInfo.imageColorSpace = surfaceFormats[0].colorSpace;
        swapInfo.imageExtent = capabilities.currentExtent;
        swapInfo.imageArrayLayers = 1;
        swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapInfo.preTransform = capabilities.currentTransform;
        swapInfo.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapInfo.clipped = VK_TRUE;
        VkSwapchainKHR swapchain{};
        if (createSwapchain(device, &swapInfo, nullptr, &swapchain) != VK_SUCCESS)
        {
            Log("[android] vkCreateSwapchainKHR failed");
            destroyDevice(device, nullptr);
            return;
        }
        uint32_t imageCount = 0;
        getImages(device, swapchain, &imageCount, nullptr);
        std::vector<VkImage> images(imageCount);
        getImages(device, swapchain, &imageCount, images.data());
        Log("[android] swapchain %ux%u, %u images, format %d, transform 0x%X", swapInfo.imageExtent.width, swapInfo.imageExtent.height,
            imageCount, int(swapInfo.imageFormat), unsigned(capabilities.currentTransform));

        VkQueue queue{};
        getQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VkCommandPool pool{};
        createPool(device, &poolInfo, nullptr, &pool);
        VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        allocate(device, &allocInfo, &cmd);
        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore acquired{}, rendered{};
        createSemaphore(device, &semaphoreInfo, nullptr, &acquired);
        createSemaphore(device, &semaphoreInfo, nullptr, &rendered);
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkFence fence{};
        createFence(device, &fenceInfo, nullptr, &fence);

        uint64_t start = SDL_GetTicks();
        uint32_t frames = 0;
        while (SDL_GetTicks() - start < uint64_t(seconds * 1000))
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
            }
            waitFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            uint32_t index = 0;
            VkResult result = acquire(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            {
                Log("[android] vkAcquireNextImageKHR: %d", int(result));
                break;
            }
            resetFences(device, 1, &fence);
            resetBuffer(cmd, 0);
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin(cmd, &beginInfo);
            VkImageMemoryBarrier toClear{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toClear.srcQueueFamilyIndex = toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.image = images[index];
            toClear.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toClear);
            float t = float(SDL_GetTicks() - start) / float(seconds * 1000);
            VkClearColorValue color{ { 1.0f, 0.45f + 0.2f * t, 0.05f, 1.0f } };
            clear(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &toClear.subresourceRange);
            VkImageMemoryBarrier toPresent = toClear;
            toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toPresent.dstAccessMask = 0;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
            end(cmd);
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &acquired;
            submitInfo.pWaitDstStageMask = &waitStage;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmd;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &rendered;
            submit(queue, 1, &submitInfo, fence);
            VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &rendered;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &swapchain;
            presentInfo.pImageIndices = &index;
            present(queue, &presentInfo);
            frames++;
        }
        waitIdle(device);
        Log("[android] presented %u frames in %.1f s", frames, double(SDL_GetTicks() - start) / 1000.0);
        // The device goes with everything made from it (the probe's objects are not reused).
        destroySwapchain(device, swapchain, nullptr);
        reinterpret_cast<PFN_vkDestroyCommandPool>(D("vkDestroyCommandPool"))(device, pool, nullptr);
        reinterpret_cast<PFN_vkDestroySemaphore>(D("vkDestroySemaphore"))(device, acquired, nullptr);
        reinterpret_cast<PFN_vkDestroySemaphore>(D("vkDestroySemaphore"))(device, rendered, nullptr);
        reinterpret_cast<PFN_vkDestroyFence>(D("vkDestroyFence"))(device, fence, nullptr);
        destroyDevice(device, nullptr);
    }

    void ProbeVulkan()
    {
        if (!SDL_Vulkan_LoadLibrary(nullptr))
        {
            Log("[android] SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
            return;
        }
        SDL_Window* window = SDL_CreateWindow("CrashRecomp", 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
        if (!window)
        {
            Log("[android] SDL_CreateWindow failed: %s", SDL_GetError());
            return;
        }
        Vk vk;
        vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        auto enumerateVersion = vk.Get<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion");
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        if (enumerateVersion)
            enumerateVersion(&loaderVersion);
        Uint32 extensionCount = 0;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "CrashRecomp";
        app.apiVersion = loaderVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : loaderVersion;
        VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = extensionCount;
        instanceInfo.ppEnabledExtensionNames = extensions;
        auto createInstance = vk.Get<PFN_vkCreateInstance>("vkCreateInstance");
        if (createInstance(&instanceInfo, nullptr, &vk.instance) != VK_SUCCESS)
        {
            Log("[android] vkCreateInstance failed (loader %u.%u)", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
            SDL_DestroyWindow(window);
            return;
        }
        VkSurfaceKHR surface{};
        if (!SDL_Vulkan_CreateSurface(window, vk.instance, nullptr, &surface))
            Log("[android] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        else
            Log("[android] Vulkan loader %u.%u, surface created", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));

        auto enumerateDevices = vk.Get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        auto properties = vk.Get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        auto features2 = vk.Get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
        auto deviceExtensions = vk.Get<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        auto formatProperties = vk.Get<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
        auto queueFamilies = vk.Get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        auto surfaceSupport = vk.Get<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR");
        uint32_t count = 0;
        enumerateDevices(vk.instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        enumerateDevices(vk.instance, &count, devices.data());
        for (VkPhysicalDevice physical : devices)
        {
            VkPhysicalDeviceProperties p{};
            properties(physical, &p);
            // The 1.0 features always; the 1.2 / 1.3 structures only where the device has them
            // (a 1.1 driver, like the S20 FE's Adreno 650, must not see them in the chain).
            VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            VkPhysicalDeviceFeatures2 f{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            if (p.apiVersion >= VK_API_VERSION_1_2)
            {
                f.pNext = &f12;
                if (p.apiVersion >= VK_API_VERSION_1_3)
                    f12.pNext = &f13;
            }
            if (p.apiVersion >= VK_API_VERSION_1_1)
                features2(physical, &f);
            else
                vk.Get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &f.features);
            uint32_t extensionTotal = 0;
            deviceExtensions(physical, nullptr, &extensionTotal, nullptr);
            std::vector<VkExtensionProperties> ext(extensionTotal);
            deviceExtensions(physical, nullptr, &extensionTotal, ext.data());
            VkFormatProperties d24{}, bc1{}, bc5{};
            formatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &d24);
            formatProperties(physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &bc1);
            formatProperties(physical, VK_FORMAT_BC5_UNORM_BLOCK, &bc5);
            Log("[android] GPU %s, vendor 0x%04X, Vulkan %u.%u.%u, driver 0x%08X", p.deviceName, p.vendorID, VK_API_VERSION_MAJOR(p.apiVersion),
                VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
            Log("[android]   geometryShader %d, textureCompressionBC %d (BC1 sampled %d, BC5 sampled %d), ETC2 %d, ASTC %d",
                int(f.features.geometryShader), int(f.features.textureCompressionBC),
                int((bc1.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0),
                int((bc5.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0),
                int(f.features.textureCompressionETC2), int(f.features.textureCompressionASTC_LDR));
            Log("[android]   dynamicRendering %d, synchronization2 %d, demoteToHelper %d, timelineSemaphore %d, depthClamp %d, "
                "anisotropy %d, independentBlend %d, D24S8 sampled depth %d",
                int(f13.dynamicRendering), int(f13.synchronization2), int(f13.shaderDemoteToHelperInvocation), int(f12.timelineSemaphore),
                int(f.features.depthClamp), int(f.features.samplerAnisotropy), int(f.features.independentBlend),
                int((d24.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) ==
                    (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)));
            Log("[android]   push_descriptor %d, graphics_pipeline_library %d, depth_clip_enable %d, depth_bias_control %d, maintenance5 %d, "
                "%u extensions, maxImageDimension2D %u",
                int(HasExtension(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)), int(HasExtension(ext, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)),
                int(HasExtension(ext, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME)), int(HasExtension(ext, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME)),
                int(HasExtension(ext, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)), extensionTotal, p.limits.maxImageDimension2D);
            Log("[android]   as extensions (pre-1.3 drivers): dynamic_rendering %d, synchronization2 %d, timeline_semaphore %d, "
                "demote_to_helper %d, extended_dynamic_state %d/%d",
                int(HasExtension(ext, "VK_KHR_dynamic_rendering")), int(HasExtension(ext, "VK_KHR_synchronization2")),
                int(HasExtension(ext, "VK_KHR_timeline_semaphore")), int(HasExtension(ext, "VK_EXT_shader_demote_to_helper_invocation")),
                int(HasExtension(ext, "VK_EXT_extended_dynamic_state")), int(HasExtension(ext, "VK_EXT_extended_dynamic_state2")));
            std::string missing;
            if (p.apiVersion < VK_API_VERSION_1_3)
                missing += " Vulkan1.3";
            if (!f13.dynamicRendering || !f13.synchronization2 || !f13.shaderDemoteToHelperInvocation || !f12.timelineSemaphore)
                missing += " core1.3-features";
            if (!f.features.geometryShader)
                missing += " geometryShader";
            if (!HasExtension(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
                missing += " push_descriptor";
            if (!(bc1.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                missing += " BC-textures";
            Log("[android]   renderer requirements: %s%s", missing.empty() ? "all present" : "missing", missing.c_str());

            if (!surface)
                continue;
            uint32_t familyCount = 0;
            queueFamilies(physical, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            queueFamilies(physical, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount; i++)
            {
                VkBool32 supported = VK_FALSE;
                surfaceSupport(physical, i, surface, &supported);
                if (supported && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                {
                    ClearScreen(vk, physical, i, surface, 3.0);
                    break;
                }
            }
            break;  // the first GPU (phones have one)
        }
        if (surface)
            vk.Get<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(vk.instance, surface, nullptr);
        vk.Get<PFN_vkDestroyInstance>("vkDestroyInstance")(vk.instance, nullptr);
        SDL_DestroyWindow(window);
    }

    // ---- The game ------------------------------------------------------------------------------
    struct GameArguments
    {
        std::vector<std::string> strings;
        std::vector<char*> argv;
    };

    void* GameThread(void* param)
    {
        auto* arguments = static_cast<GameArguments*>(param);
        int code = CrashMain(int(arguments->argv.size()) - 1, arguments->argv.data());
        Log("[android] the game returned %d", code);
        std::lock_guard lock(g_jobsMutex);
        g_gameFinished = true;
        g_gameExitCode = code;
        g_jobsChanged.notify_all();
        return nullptr;
    }
}

namespace plat::android
{
    void RunOnMainThread(std::function<void()> job)
    {
        std::lock_guard lock(g_jobsMutex);
        g_jobs.push_back(std::move(job));
        g_jobsChanged.notify_all();
    }
}

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    const char* external = SDL_GetAndroidExternalStoragePath();
    if (!external)
    {
        __android_log_print(ANDROID_LOG_ERROR, "CrashRecomp", "no external files folder: %s", SDL_GetError());
        return 1;
    }
    std::filesystem::path files = external;
    setenv("CRASH_ANDROID_FILES", external, 1);
    std::error_code error;
    for (const char* folder : { "game", "userdata", "mods", "data", "languages" })
        std::filesystem::create_directories(files / folder, error);

    Log("[android] %s %s (%s), Android %s (API %d), SoC %s %s, page size %ld, %ld cores", Property("ro.product.manufacturer").c_str(),
        Property("ro.product.model").c_str(), Property("ro.product.device").c_str(), Property("ro.build.version.release").c_str(),
        SDL_GetAndroidSDKVersion(), Property("ro.soc.manufacturer").c_str(), Property("ro.soc.model").c_str(), sysconf(_SC_PAGESIZE),
        sysconf(_SC_NPROCESSORS_CONF));
    Log("[android] files folder: %s", external);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
        Log("[android] SDL_Init failed: %s", SDL_GetError());
    ProbeVulkan();

    if (!std::filesystem::is_regular_file(files / "game" / "default.xex", error))
    {
        std::string text = "Copie o dump do Crash of the Titans (default.xex, default.rcf...) para\n" + (files / "game").string() +
                           "\n\nO teste de Vulkan terminou: veja o logcat (tag CrashRecomp).";
        Log("[android] no game in %s: stopping after the Vulkan probe", (files / "game").c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "CrashRecomp", text.c_str(), nullptr);
        SDL_Quit();
        return 0;
    }

    // Arguments: args.txt in the files folder, one per line.
    auto* arguments = new GameArguments();
    arguments->strings.push_back("CrashRecomp");
    std::ifstream argsFile(files / "args.txt");
    for (std::string line; std::getline(argsFile, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && line[0] != '#')
            arguments->strings.push_back(line);
    }
    for (std::string& s : arguments->strings)
        arguments->argv.push_back(s.data());
    arguments->argv.push_back(nullptr);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 64u << 20);
    pthread_t game;
    int result = pthread_create(&game, &attributes, GameThread, arguments);
    pthread_attr_destroy(&attributes);
    if (result != 0)
    {
        Log("[android] could not start the game thread (%d)", result);
        return 1;
    }

    // The SDL thread: jobs from the game (the window's loop, which then keeps this thread).
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(g_jobsMutex);
            g_jobsChanged.wait(lock, [] { return !g_jobs.empty() || g_gameFinished; });
            if (g_jobs.empty())
                break;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
    }
    SDL_Quit();
    return g_gameExitCode;
}
