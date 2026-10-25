// CrashDlss.dll (crash_dlss.h): DLSS through NGX's Vulkan API, for the renderer (upscaler.cpp) and
// for the launcher's and the installer's "can this PC run DLSS?" (CrashDlss_Probe).
#include "crash_dlss.h"

#include <windows.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define CRASH_DLSS_EXPORT extern "C" __declspec(dllexport)

namespace
{
    // Project id required by NVSDK_NGX_VULKAN_Init_with_ProjectID for custom engines (any GUID-like
    // string; this one is CrashRecomp's).
    constexpr const char* NGX_PROJECT_ID = "6c7a3f1e-0b58-4d6e-9a3c-56550e4c7261";

    CrashDlssLogFn g_log = nullptr;
    VkDevice g_device = VK_NULL_HANDLE;
    bool g_initialized = false;
    NVSDK_NGX_Parameter* g_parameters = nullptr;
    NVSDK_NGX_Handle* g_feature = nullptr;
    uint32_t g_featureKey[5] = {};  // render w/h, output w/h, quality + flags of the feature

    void Log(const char* format, ...)
    {
        if (!g_log)
            return;
        char text[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        g_log(text);
    }

    void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        std::string text = message ? message : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        Log("[dlss] %s", text.c_str());
    }

    // NGX on `device`, its capability parameters, and whether DLSS Super Resolution is available.
    // `why` gets the reason when it isn't. Leaves NGX initialized (g_initialized / g_parameters).
    bool StartNgx(const CrashDlssDevice& device, std::string& why)
    {
        const wchar_t* paths[] = { device.featureDir ? device.featureDir : L"." };
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
        info.LoggingInfo.LoggingCallback = NgxLog;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        const wchar_t* dataDir = device.dataDir && *device.dataDir ? device.dataDir : paths[0];
        CreateDirectoryW(dataDir, nullptr);
        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataDir,
                                                                        device.instance, device.physical, device.device,
                                                                        device.getInstanceProcAddr, device.getDeviceProcAddr, &info,
                                                                        NVSDK_NGX_Version_API);
        char text[160];
        if (NVSDK_NGX_FAILED(result))
        {
            snprintf(text, sizeof(text), "NGX initialization failed (0x%08X; not an NVIDIA RTX GPU or an old driver)", uint32_t(result));
            why = text;
            return false;
        }
        g_initialized = true;
        g_device = device.device;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_parameters)) || !g_parameters)
        {
            g_parameters = nullptr;
            why = "no NGX capability parameters";
            return false;
        }
        int available = 0, needsDriver = 0;
        NVSDK_NGX_Parameter_GetI(g_parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(g_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        if (!available)
        {
            int initResult = 0;
            NVSDK_NGX_Parameter_GetI(g_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
            snprintf(text, sizeof(text), "not available on this GPU (needs updated driver: %s, result 0x%08X)", needsDriver ? "yes" : "no",
                     uint32_t(initResult));
            why = text;
            return false;
        }
        return true;
    }

    void StopNgx()
    {
        if (g_feature)
            NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = nullptr;
        if (g_parameters)
            NVSDK_NGX_VULKAN_DestroyParameters(g_parameters);
        g_parameters = nullptr;
        if (g_initialized)
            NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_initialized = false;
        g_device = VK_NULL_HANDLE;
    }

    NVSDK_NGX_PerfQuality_Value Quality(int32_t quality)
    {
        switch (quality)
        {
        case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case 3: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case 4: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        default: return NVSDK_NGX_PerfQuality_Value_DLAA;
        }
    }

    const char* QualityName(int32_t quality)
    {
        static const char* names[] = { "native", "quality", "balanced", "performance", "ultra_performance" };
        return quality >= 0 && quality < 5 ? names[quality] : "?";
    }

    NVSDK_NGX_Resource_VK Resource(const CrashDlssImage& image, bool readWrite)
    {
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format, image.width, image.height, readWrite);
    }

    void Copy(char* out, uint32_t size, const std::string& text)
    {
        if (out && size)
            snprintf(out, size, "%s", text.c_str());
    }
}

CRASH_DLSS_EXPORT uint32_t CrashDlss_ApiVersion()
{
    return CRASH_DLSS_API_VERSION;
}

CRASH_DLSS_EXPORT int32_t CrashDlss_RequiredExtensions(uint32_t* instanceCount, const char* const** instanceNames, uint32_t* deviceCount,
                                                       const char* const** deviceNames)
{
    // NGX's list (the same for every NVIDIA GPU; the device enables the ones the driver has).
    unsigned int ic = 0, dc = 0;
    const char** in = nullptr;
    const char** dn = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ic, &in, &dc, &dn)))
        return 0;
    *instanceCount = ic, *instanceNames = in, *deviceCount = dc, *deviceNames = dn;
    return 1;
}

CRASH_DLSS_EXPORT int32_t CrashDlss_Init(const CrashDlssDevice* device)
{
    g_log = device->log;
    std::string why;
    if (StartNgx(*device, why))
        return 1;
    Log("[gpu] DLSS: %s", why.c_str());
    return 0;
}

CRASH_DLSS_EXPORT int32_t CrashDlss_Evaluate(const CrashDlssFrame* frame)
{
    if (!g_parameters)
        return 0;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                (frame->depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    uint32_t key[5] = { frame->renderWidth, frame->renderHeight, frame->outputWidth, frame->outputHeight,
                        uint32_t(frame->quality) | (uint32_t(flags) << 8) };
    if (g_feature && memcmp(key, g_featureKey, sizeof(key)) != 0)
    {
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    if (!g_feature)
    {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = frame->renderWidth;
        create.Feature.InHeight = frame->renderHeight;
        create.Feature.InTargetWidth = frame->outputWidth;
        create.Feature.InTargetHeight = frame->outputHeight;
        create.Feature.InPerfQualityValue = Quality(frame->quality);
        create.InFeatureCreateFlags = flags;
        // Presets: the DLL's defaults (transformer K for DLAA/quality/balanced, M/L for the
        // performance modes in DLSS 4.5).
        NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, frame->commandBuffer, 1, 1, &g_feature, g_parameters, &create);
        if (NVSDK_NGX_FAILED(result))
        {
            static uint32_t logged;
            if (logged++ < 4)
                Log("[gpu] DLSS: could not create the feature %ux%u -> %ux%u (0x%08X)", frame->renderWidth, frame->renderHeight,
                    frame->outputWidth, frame->outputHeight, uint32_t(result));
            g_feature = nullptr;
            return 0;
        }
        memcpy(g_featureKey, key, sizeof(key));
        Log("[gpu] DLSS feature created: %ux%u -> %ux%u, %s", frame->renderWidth, frame->renderHeight, frame->outputWidth,
            frame->outputHeight, QualityName(frame->quality));
    }
    NVSDK_NGX_Resource_VK color = Resource(frame->color, false), depth = Resource(frame->depth, false),
                          motion = Resource(frame->motion, false), output = Resource(frame->output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = frame->jitterX;
    eval.InJitterOffsetY = frame->jitterY;
    eval.InRenderSubrectDimensions = { frame->renderWidth, frame->renderHeight };
    eval.InReset = frame->reset ? 1 : 0;
    // Motion vectors are in UV units: scale to render pixels.
    eval.InMVScaleX = float(frame->renderWidth);
    eval.InMVScaleY = float(frame->renderHeight);
    eval.InFrameTimeDeltaInMsec = frame->frameTimeMs;
    NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(frame->commandBuffer, g_feature, g_parameters, &eval);
    if (NVSDK_NGX_FAILED(result))
    {
        static uint32_t logged;
        if (logged++ < 4)
            Log("[gpu] DLSS: evaluation failed (0x%08X)", uint32_t(result));
        return 0;
    }
    return 1;
}

CRASH_DLSS_EXPORT void CrashDlss_ReleaseFeature()
{
    if (g_feature)
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
    g_feature = nullptr;
}

CRASH_DLSS_EXPORT void CrashDlss_Shutdown()
{
    StopNgx();
}

CRASH_DLSS_EXPORT int32_t CrashDlss_Probe(const wchar_t* featureDir, const wchar_t* dataDir, char* gpuName, uint32_t gpuNameSize, char* reason,
                                          uint32_t reasonSize)
{
    Copy(gpuName, gpuNameSize, "");
    Copy(reason, reasonSize, "");
    if (g_initialized)
    {
        Copy(reason, reasonSize, "NGX is already in use in this process");
        return 0;
    }
    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan)
    {
        Copy(reason, reasonSize, "no Vulkan driver (vulkan-1.dll)");
        return 0;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    auto createInstance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    auto enumerateInstanceExtensions =
        gipa ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties")) : nullptr;
    if (!createInstance || !enumerateInstanceExtensions)
    {
        FreeLibrary(vulkan);
        Copy(reason, reasonSize, "vulkan-1.dll has no vkCreateInstance");
        return 0;
    }
    unsigned int ngxInstanceCount = 0, ngxDeviceCount = 0;
    const char** ngxInstance = nullptr;
    const char** ngxDevice = nullptr;
    NVSDK_NGX_VULKAN_RequiredExtensions(&ngxInstanceCount, &ngxInstance, &ngxDeviceCount, &ngxDevice);

    // The instance, with the NGX instance extensions the loader has.
    uint32_t count = 0;
    enumerateInstanceExtensions(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> have(count);
    enumerateInstanceExtensions(nullptr, &count, have.data());
    auto has = [](const std::vector<VkExtensionProperties>& list, const char* name) {
        for (const VkExtensionProperties& e : list)
            if (strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };
    std::vector<const char*> instanceExtensions;
    for (unsigned int i = 0; i < ngxInstanceCount; i++)
        if (has(have, ngxInstance[i]))
            instanceExtensions.push_back(ngxInstance[i]);
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "CrashRecomp DLSS probe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = uint32_t(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
    {
        FreeLibrary(vulkan);
        Copy(reason, reasonSize, "vkCreateInstance failed (no Vulkan 1.3 driver)");
        return 0;
    }
#define INSTANCE_FN(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
    INSTANCE_FN(vkDestroyInstance);
    INSTANCE_FN(vkEnumeratePhysicalDevices);
    INSTANCE_FN(vkGetPhysicalDeviceProperties);
    INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);
#undef INSTANCE_FN

    // The NVIDIA GPU (a discrete one first).
    count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties chosen{};
    for (VkPhysicalDevice candidate : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID != 0x10DE)
            continue;
        if (!physical || (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && chosen.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
            physical = candidate, chosen = properties;
    }
    int32_t available = 0;
    std::string why;
    if (!physical)
        why = "no NVIDIA GPU";
    else
    {
        Copy(gpuName, gpuNameSize, chosen.deviceName);
        count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < count && family == UINT32_MAX; i++)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                family = i;
        count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> deviceHave(count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, deviceHave.data());
        std::vector<const char*> deviceExtensions;
        for (unsigned int i = 0; i < ngxDeviceCount; i++)
            // VK_EXT_buffer_device_address: the core feature replaces it (both together are invalid).
            if (strcmp(ngxDevice[i], "VK_EXT_buffer_device_address") != 0 && has(deviceHave, ngxDevice[i]))
                deviceExtensions.push_back(ngxDevice[i]);
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue.queueFamilyIndex = family == UINT32_MAX ? 0 : family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.bufferDeviceAddress = VK_TRUE;  // NGX's features want device addresses
        VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features12 };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queue;
        deviceInfo.enabledExtensionCount = uint32_t(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        VkDevice device = VK_NULL_HANDLE;
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
            why = "vkCreateDevice failed on the NVIDIA GPU";
        else
        {
            CrashDlssDevice info{ instance, physical, device, gipa, vkGetDeviceProcAddr, dataDir, featureDir, nullptr };
            available = StartNgx(info, why) ? 1 : 0;
            StopNgx();
            auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(device, "vkDestroyDevice"));
            vkDestroyDevice(device, nullptr);
        }
    }
    vkDestroyInstance(instance, nullptr);
    FreeLibrary(vulkan);
    Copy(reason, reasonSize, available ? std::string("DLSS available") : why);
    return available;
}
