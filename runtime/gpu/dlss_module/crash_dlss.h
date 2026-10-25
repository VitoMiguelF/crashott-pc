#pragma once

// CrashDlss.dll: NVIDIA DLSS Super Resolution / DLAA for CrashRecomp, behind a small C interface.
//
// The DLL is our code (crash_dlss.cpp, MIT) linked with NVIDIA's static NGX library
// (thirdparty/dlss/lib/nvsdk_ngx_s.lib) and loaded at run time, next to nvngx_dlss.dll:
//   - CrashRecomp.exe (gpu/upscaler.cpp) runs DLSS on the renderer's own Vulkan device;
//   - CrashLauncher.exe and CrashSetup.exe ask CrashDlss_Probe whether this PC can run DLSS (an
//     NVIDIA RTX GPU whose driver has DLSS), through NGX's own capability check on a Vulkan device
//     of their own, so that DLSS is offered only where it works.
// The installer carries it prebuilt (with nvngx_dlss.dll), so building the game needs no part of
// the DLSS SDK: the SDK's files reach players only in object code form inside this DLL, as its
// license allows. A clone with thirdparty/dlss builds it (CMake target CrashDlss).

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <cstdint>

#define CRASH_DLSS_API_VERSION 1

extern "C"
{
    typedef void (*CrashDlssLogFn)(const char* text);

    struct CrashDlssDevice
    {
        VkInstance instance;
        VkPhysicalDevice physical;
        VkDevice device;
        PFN_vkGetInstanceProcAddr getInstanceProcAddr;
        PFN_vkGetDeviceProcAddr getDeviceProcAddr;
        const wchar_t* dataDir;     // where NGX may write its logs
        const wchar_t* featureDir;  // the folder with nvngx_dlss.dll
        CrashDlssLogFn log;         // NGX's and our messages (may be null)
    };

    struct CrashDlssImage
    {
        VkImage image;
        VkImageView view;
        VkFormat format;
        uint32_t width, height;
    };

    // Quality: 0 native (DLAA), 1 quality, 2 balanced, 3 performance, 4 ultra performance
    // (settings::UpscalerQuality).
    struct CrashDlssFrame
    {
        VkCommandBuffer commandBuffer;
        CrashDlssImage color, depth, motion, output;  // inputs SHADER_READ_ONLY_OPTIMAL, output GENERAL
        uint32_t renderWidth, renderHeight, outputWidth, outputHeight;
        float jitterX, jitterY;  // render pixels
        float frameTimeMs;
        int32_t quality;
        int32_t reset;
        int32_t depthInverted;
    };

    typedef uint32_t (*CrashDlss_ApiVersionFn)();
    // NGX's Vulkan extensions (instance, device), before the device is created. 1 on success.
    typedef int32_t (*CrashDlss_RequiredExtensionsFn)(uint32_t* instanceCount, const char* const** instanceNames, uint32_t* deviceCount,
                                                      const char* const** deviceNames);
    // NGX on that device; 1 if DLSS can run on it (else the log says why).
    typedef int32_t (*CrashDlss_InitFn)(const CrashDlssDevice* device);
    // Records one upscale on frame->commandBuffer (the feature is created / recreated as needed). 1 on success.
    typedef int32_t (*CrashDlss_EvaluateFn)(const CrashDlssFrame* frame);
    typedef void (*CrashDlss_ReleaseFeatureFn)();  // frees the feature (GPU idle)
    typedef void (*CrashDlss_ShutdownFn)();        // and NGX
    // Standalone: a Vulkan instance and device of its own on this PC's NVIDIA GPU, NGX's capability
    // check, then everything is destroyed again. 1 if DLSS can run; gpuName gets the GPU (or ""),
    // reason why not. featureDir: the folder with nvngx_dlss.dll; dataDir: NGX's logs (may be null).
    typedef int32_t (*CrashDlss_ProbeFn)(const wchar_t* featureDir, const wchar_t* dataDir, char* gpuName, uint32_t gpuNameSize, char* reason,
                                         uint32_t reasonSize);
}
