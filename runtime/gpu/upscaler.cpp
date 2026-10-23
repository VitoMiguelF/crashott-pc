#include "upscaler.h"
#include "../diagnostics.h"

#include <windows.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#if CRASH_HAVE_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif
#if CRASH_HAVE_FSR
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

namespace gpu::upscaler
{
    namespace
    {
        rhi::Device* g_rhi = nullptr;
        // The Vulkan objects the SDKs work on (the renderer's own device).
        VkInstance g_instance = VK_NULL_HANDLE;
        VkPhysicalDevice g_physical = VK_NULL_HANDLE;
        VkDevice g_device = VK_NULL_HANDLE;
        PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;
        PFN_vkGetDeviceProcAddr g_getDeviceProcAddr = nullptr;
        std::wstring g_dataDir;
        bool g_dlssAvailable = false, g_fsrAvailable = false;
        // Probing loads the SDK's DLLs (NGX initialization, the FSR provider): only the upscaler
        // chosen in settings.toml is probed at startup, the other one when the settings screen first
        // asks (Available).
        bool g_dlssProbed = false, g_fsrProbed = false;
        settings::Upscaler g_active = settings::Upscaler::Off;
        std::string g_name = "off";

        std::filesystem::path ExeDirectory()
        {
            wchar_t path[MAX_PATH];
            DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            return std::filesystem::path(std::wstring(path, length)).parent_path();
        }

        // "a.b.c" from a DLL's version resource, or "" if it has none.
        std::string FileVersion(const std::filesystem::path& file)
        {
            DWORD handle = 0;
            DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
            if (!size)
                return "";
            std::vector<uint8_t> data(size);
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
                return "";
            char text[64];
            snprintf(text, sizeof(text), "%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS));
            return text;
        }

        float Halton(uint32_t index, uint32_t base)
        {
            float f = 1.0f, result = 0.0f;
            for (uint32_t i = index; i > 0; i /= base)
            {
                f /= float(base);
                result += f * float(i % base);
            }
            return result;
        }

        // A texture of a frame as the SDKs see it: image, view and size, in the layout they expect
        // (inputs SHADER_READ_ONLY_OPTIMAL, the output GENERAL).
        struct NativeTexture
        {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
            uint32_t width = 0, height = 0;
        };

        struct NativeFrame
        {
            VkCommandBuffer commandBuffer;
            NativeTexture color, depth, motion, output;
        };

        VkFormat VulkanFormat(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
            case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        NativeTexture Native(rhi::Texture* texture, rhi::NativeState state)
        {
            g_rhi->PrepareNative(texture, state);
            NativeTexture native;
            native.image = static_cast<VkImage>(g_rhi->NativeTexture(texture));
            native.view = static_cast<VkImageView>(g_rhi->NativeView(texture));
            native.dxgi = texture->desc.format;
            native.format = VulkanFormat(texture->desc.format);
            native.width = texture->desc.width;
            native.height = texture->desc.height;
            return native;
        }

        NativeFrame Natives(const Frame& frame)
        {
            NativeFrame native;
            native.color = Native(frame.color, rhi::NativeState::ShaderRead);
            native.depth = Native(frame.depth, rhi::NativeState::ShaderRead);
            native.motion = Native(frame.motion, rhi::NativeState::ShaderRead);
            native.output = Native(frame.output, rhi::NativeState::UnorderedAccess);
            // Native() ends the rendering instance and flushes the barriers above.
            native.commandBuffer = static_cast<VkCommandBuffer>(g_rhi->Native().commandList);
            return native;
        }

#if CRASH_HAVE_DLSS
        // NGX ----------------------------------------------------------------------------------
        // Project id required by NVSDK_NGX_VULKAN_Init_with_ProjectID for custom engines (any
        // GUID-like string; this one is CrashRecomp's).
        constexpr const char* NGX_PROJECT_ID = "6c7a3f1e-0b58-4d6e-9a3c-56550e4c7261";
        bool g_ngxInitialized = false;
        NVSDK_NGX_Parameter* g_ngxParameters = nullptr;
        NVSDK_NGX_Handle* g_dlss = nullptr;
        uint32_t g_dlssKey[5] = {};  // render w/h, output w/h, quality + flags of the feature

        void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
        {
            std::string text = message ? message : "";
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            Log("[dlss] %s", text.c_str());
        }

        void ProbeDlss(const std::wstring& dataDir)
        {
            std::filesystem::path dll = ExeDirectory() / "nvngx_dlss.dll";
            std::error_code error;
            if (!std::filesystem::exists(dll, error))
            {
                Log("[gpu] DLSS: nvngx_dlss.dll not found next to the exe");
                return;
            }
            std::wstring exeDir = ExeDirectory().wstring();
            const wchar_t* paths[] = { exeDir.c_str() };
            NVSDK_NGX_FeatureCommonInfo info{};
            info.PathListInfo.Path = paths;
            info.PathListInfo.Length = 1;
            info.LoggingInfo.LoggingCallback = NgxLog;
            info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
            std::filesystem::create_directories(dataDir, error);
            NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataDir.c_str(),
                                                                            g_instance, g_physical, g_device, g_getInstanceProcAddr,
                                                                            g_getDeviceProcAddr, &info, NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(result))
            {
                Log("[gpu] DLSS: NGX initialization failed (0x%08X; not an NVIDIA RTX GPU or an old driver)", uint32_t(result));
                return;
            }
            g_ngxInitialized = true;
            if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_ngxParameters)) || !g_ngxParameters)
            {
                Log("[gpu] DLSS: no NGX capability parameters");
                return;
            }
            int available = 0, needsDriver = 0;
            NVSDK_NGX_Parameter_GetI(g_ngxParameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            NVSDK_NGX_Parameter_GetI(g_ngxParameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            if (!available)
            {
                int initResult = 0;
                NVSDK_NGX_Parameter_GetI(g_ngxParameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
                Log("[gpu] DLSS: not available on this GPU (needs updated driver: %s, result 0x%08X)", needsDriver ? "yes" : "no", uint32_t(initResult));
                return;
            }
            g_dlssAvailable = true;
            std::string version = FileVersion(dll);
            Log("[gpu] DLSS available (nvngx_dlss.dll %s)", version.empty() ? "?" : version.c_str());
        }

        NVSDK_NGX_PerfQuality_Value DlssQuality(settings::UpscalerQuality quality)
        {
            switch (quality)
            {
            case settings::UpscalerQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            case settings::UpscalerQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
            case settings::UpscalerQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            case settings::UpscalerQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            default: return NVSDK_NGX_PerfQuality_Value_DLAA;
            }
        }

        NVSDK_NGX_Resource_VK DlssResource(const NativeTexture& texture, bool readWrite)
        {
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            return NVSDK_NGX_Create_ImageView_Resource_VK(texture.view, texture.image, range, texture.format, texture.width, texture.height, readWrite);
        }

        bool EvaluateDlss(const Frame& frame)
        {
            NativeFrame native = Natives(frame);
            int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                        (frame.depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
            uint32_t key[5] = { frame.renderWidth, frame.renderHeight, frame.outputWidth, frame.outputHeight, uint32_t(frame.quality) | (uint32_t(flags) << 8) };
            if (g_dlss && memcmp(key, g_dlssKey, sizeof(key)) != 0)
            {
                NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss);
                g_dlss = nullptr;
            }
            if (!g_dlss)
            {
                NVSDK_NGX_DLSS_Create_Params create{};
                create.Feature.InWidth = frame.renderWidth;
                create.Feature.InHeight = frame.renderHeight;
                create.Feature.InTargetWidth = frame.outputWidth;
                create.Feature.InTargetHeight = frame.outputHeight;
                create.Feature.InPerfQualityValue = DlssQuality(frame.quality);
                create.InFeatureCreateFlags = flags;
                // Presets: the DLL's defaults (transformer K for DLAA/quality/balanced, M/L for the
                // performance modes in DLSS 4.5).
                NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, native.commandBuffer, 1, 1, &g_dlss, g_ngxParameters, &create);
                if (NVSDK_NGX_FAILED(result))
                {
                    static uint32_t logged;
                    if (logged++ < 4)
                        Log("[gpu] DLSS: could not create the feature %ux%u -> %ux%u (0x%08X)", frame.renderWidth, frame.renderHeight,
                            frame.outputWidth, frame.outputHeight, uint32_t(result));
                    g_dlss = nullptr;
                    return false;
                }
                memcpy(g_dlssKey, key, sizeof(key));
                Log("[gpu] DLSS feature created: %ux%u -> %ux%u, %s", frame.renderWidth, frame.renderHeight, frame.outputWidth, frame.outputHeight,
                    settings::UpscalerQualityName(frame.quality));
            }
            NVSDK_NGX_Resource_VK color = DlssResource(native.color, false), depth = DlssResource(native.depth, false),
                                  motion = DlssResource(native.motion, false), output = DlssResource(native.output, true);
            NVSDK_NGX_VK_DLSS_Eval_Params eval{};
            eval.Feature.pInColor = &color;
            eval.Feature.pInOutput = &output;
            eval.pInDepth = &depth;
            eval.pInMotionVectors = &motion;
            eval.InJitterOffsetX = frame.jitterX;
            eval.InJitterOffsetY = frame.jitterY;
            eval.InRenderSubrectDimensions = { frame.renderWidth, frame.renderHeight };
            eval.InReset = frame.reset ? 1 : 0;
            // Motion vectors are in UV units: scale to render pixels.
            eval.InMVScaleX = float(frame.renderWidth);
            eval.InMVScaleY = float(frame.renderHeight);
            eval.InFrameTimeDeltaInMsec = frame.frameTimeMs;
            NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(native.commandBuffer, g_dlss, g_ngxParameters, &eval);
            if (NVSDK_NGX_FAILED(result))
            {
                static uint32_t logged;
                if (logged++ < 4)
                    Log("[gpu] DLSS: evaluation failed (0x%08X)", uint32_t(result));
                return false;
            }
            return true;
        }
#endif

#if CRASH_HAVE_FSR
        // FidelityFX API ------------------------------------------------------------------------
        HMODULE g_ffxModule = nullptr;
        // The FidelityFX API entry points of amd_fidelityfx_vk.dll.
        struct
        {
            PfnFfxCreateContext CreateContext;
            PfnFfxDestroyContext DestroyContext;
            PfnFfxConfigure Configure;
            PfnFfxQuery Query;
            PfnFfxDispatch Dispatch;
        } g_ffx{};
        ffxContext g_fsr = nullptr;
        uint64_t g_fsrVersionId = 0;   // the FSR 3.1 provider, if listed
        std::string g_fsrVersionName;
        uint32_t g_fsrKey[4] = {};     // max render / output size of the context
        uint32_t g_fsrFlags = 0;

        void FfxMessage(uint32_t type, const wchar_t* message)
        {
            char text[512];
            WideCharToMultiByte(CP_UTF8, 0, message ? message : L"", -1, text, sizeof(text), nullptr, nullptr);
            Log("[fsr] %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
        }

        void ProbeFsr()
        {
            std::filesystem::path dll = ExeDirectory() / "amd_fidelityfx_vk.dll";
            g_ffxModule = LoadLibraryW(dll.c_str());
            if (!g_ffxModule)
            {
                Log("[gpu] FSR: amd_fidelityfx_vk.dll not found next to the exe");
                return;
            }
            g_ffx.CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_ffxModule, "ffxCreateContext"));
            g_ffx.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_ffxModule, "ffxDestroyContext"));
            g_ffx.Configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_ffxModule, "ffxConfigure"));
            g_ffx.Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_ffxModule, "ffxQuery"));
            g_ffx.Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_ffxModule, "ffxDispatch"));
            if (!g_ffx.CreateContext || !g_ffx.Query || !g_ffx.Dispatch || !g_ffx.DestroyContext)
            {
                Log("[gpu] FSR: amd_fidelityfx_vk.dll lacks the FidelityFX API entry points");
                return;
            }
            // Upscaler versions this DLL has (the device is only needed on DX12).
            uint64_t count = 0;
            ffxQueryDescGetVersions versions{};
            versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            versions.outputCount = &count;
            if (g_ffx.Query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count)
            {
                Log("[gpu] FSR: no upscaler versions available");
                return;
            }
            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data();
            versions.versionNames = names.data();
            g_ffx.Query(nullptr, &versions.header);
            std::string list;
            for (uint64_t i = 0; i < count; i++)
            {
                list += std::string(i ? ", " : "") + (names[i] ? names[i] : "?");
                // The UI promises FSR 3.1: prefer the newest 3.1.x provider.
                if (names[i] && strncmp(names[i], "3.1", 3) == 0 && (g_fsrVersionName.empty() || strcmp(names[i], g_fsrVersionName.c_str()) > 0))
                {
                    g_fsrVersionId = ids[i];
                    g_fsrVersionName = names[i];
                }
            }
            if (!g_fsrVersionId)
            {
                g_fsrVersionId = ids[0];
                g_fsrVersionName = names[0] ? names[0] : "?";
            }
            g_fsrAvailable = true;
            Log("[gpu] FSR available: upscaler versions %s; using %s", list.c_str(), g_fsrVersionName.c_str());
        }

        uint32_t FfxFormat(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R16G16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            case DXGI_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
            default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
            }
        }

        // Described by hand: ffxApiGetImageResourceDescriptionVK reads a mutable-format image as sRGB.
        FfxApiResource FfxResource(const NativeTexture& texture, bool output)
        {
            FfxApiResourceDescription description{};
            description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            description.format = FfxFormat(texture.dxgi);
            description.width = texture.width;
            description.height = texture.height;
            description.depth = 1;
            description.mipCount = 1;
            description.flags = FFX_API_RESOURCE_FLAGS_NONE;
            description.usage = output ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
            return ffxApiGetResourceVK(reinterpret_cast<void*>(texture.image), description,
                                       output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }

        bool EvaluateFsr(const Frame& frame)
        {
            NativeFrame native = Natives(frame);
            uint32_t flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE |
                             (frame.depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u);
            uint32_t key[4] = { frame.renderWidth, frame.renderHeight, frame.outputWidth, frame.outputHeight };
            if (g_fsr && (memcmp(key, g_fsrKey, sizeof(key)) != 0 || flags != g_fsrFlags))
            {
                g_ffx.DestroyContext(&g_fsr, nullptr);
                g_fsr = nullptr;
            }
            if (!g_fsr)
            {
                ffxCreateBackendVKDesc backend{};
                backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
                backend.vkDevice = g_device;
                backend.vkPhysicalDevice = g_physical;
                backend.vkDeviceProcAddr = g_getDeviceProcAddr;
                ffxOverrideVersion version{};
                version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                version.versionId = g_fsrVersionId;
                backend.header.pNext = &version.header;
                ffxCreateContextDescUpscale create{};
                create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                create.header.pNext = &backend.header;
                create.flags = flags;
                create.maxRenderSize = { frame.renderWidth, frame.renderHeight };
                create.maxUpscaleSize = { frame.outputWidth, frame.outputHeight };
                create.fpMessage = FfxMessage;
                ffxReturnCode_t result = g_ffx.CreateContext(&g_fsr, &create.header, nullptr);
                if (result != FFX_API_RETURN_OK)
                {
                    static uint32_t logged;
                    if (logged++ < 4)
                        Log("[gpu] FSR: could not create the upscaler %ux%u -> %ux%u (%u)", frame.renderWidth, frame.renderHeight,
                            frame.outputWidth, frame.outputHeight, uint32_t(result));
                    g_fsr = nullptr;
                    return false;
                }
                memcpy(g_fsrKey, key, sizeof(key));
                g_fsrFlags = flags;
                Log("[gpu] FSR %s upscaler created: %ux%u -> %ux%u", g_fsrVersionName.c_str(), frame.renderWidth, frame.renderHeight,
                    frame.outputWidth, frame.outputHeight);
            }
            ffxDispatchDescUpscale dispatch{};
            dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dispatch.commandList = native.commandBuffer;
            dispatch.color = FfxResource(native.color, false);
            dispatch.depth = FfxResource(native.depth, false);
            dispatch.motionVectors = FfxResource(native.motion, false);
            dispatch.output = FfxResource(native.output, true);
            dispatch.jitterOffset = { frame.jitterX, frame.jitterY };
            dispatch.motionVectorScale = { float(frame.renderWidth), float(frame.renderHeight) };
            dispatch.renderSize = { frame.renderWidth, frame.renderHeight };
            dispatch.upscaleSize = { frame.outputWidth, frame.outputHeight };
            dispatch.enableSharpening = frame.sharpness > 0.0f;
            dispatch.sharpness = frame.sharpness;
            dispatch.frameTimeDelta = frame.frameTimeMs;
            dispatch.preExposure = 1.0f;
            dispatch.reset = frame.reset;
            dispatch.cameraNear = frame.cameraNear;
            dispatch.cameraFar = frame.cameraFar;
            dispatch.cameraFovAngleVertical = frame.fovY;
            dispatch.viewSpaceToMetersFactor = 1.0f;
            dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
            ffxReturnCode_t result = g_ffx.Dispatch(&g_fsr, &dispatch.header);
            if (result != FFX_API_RETURN_OK)
            {
                static uint32_t logged;
                if (logged++ < 4)
                    Log("[gpu] FSR: dispatch failed (%u)", uint32_t(result));
                return false;
            }
            return true;
        }
#endif
    }

    void VulkanExtensions(std::vector<std::string>& instance, std::vector<std::string>& device)
    {
#if CRASH_HAVE_DLSS
        // NGX's list (the same for every NVIDIA GPU; the device enables the ones the driver has).
        unsigned int instanceCount = 0, deviceCount = 0;
        const char** instanceNames = nullptr;
        const char** deviceNames = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instanceNames, &deviceCount, &deviceNames)))
        {
            for (unsigned int i = 0; i < instanceCount; i++)
                instance.push_back(instanceNames[i]);
            for (unsigned int i = 0; i < deviceCount; i++)
                device.push_back(deviceNames[i]);
        }
#endif
#if CRASH_HAVE_FSR
        // amd_fidelityfx_vk.dll looks these entry points up by their extension names
        // (vkGetBufferMemoryRequirements2KHR, vkSetDebugUtilsObjectNameEXT...), which
        // vkGetDeviceProcAddr only resolves with the extension enabled: a null call otherwise.
        instance.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        for (const char* name : { VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
                                  VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME })
            device.push_back(name);
#endif
        (void)instance;
        (void)device;
    }

    bool Available(settings::Upscaler upscaler)
    {
        if (!g_device)
            return false;
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceCounter(&start);
        bool probed = false;
        if (upscaler == settings::Upscaler::Dlss && !g_dlssProbed)
        {
            g_dlssProbed = probed = true;
#if CRASH_HAVE_DLSS
            ProbeDlss(g_dataDir);
#else
            Log("[gpu] DLSS: not built in (thirdparty/dlss missing)");
#endif
        }
        if (upscaler == settings::Upscaler::Fsr && !g_fsrProbed)
        {
            g_fsrProbed = probed = true;
#if CRASH_HAVE_FSR
            ProbeFsr();
#else
            Log("[gpu] FSR: not built in (thirdparty/fsr missing)");
#endif
        }
        if (probed)
        {
            QueryPerformanceCounter(&end);
            QueryPerformanceFrequency(&frequency);
            Log("[gpu] %s probed in %.0f ms", settings::UpscalerName(upscaler), double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart));
        }
        switch (upscaler)
        {
        case settings::Upscaler::Dlss: return g_dlssAvailable;
        case settings::Upscaler::Fsr: return g_fsrAvailable;
        default: return true;
        }
    }

    void Init(rhi::Device* device, settings::Upscaler wanted, const std::wstring& dataDir)
    {
        g_rhi = device;
        rhi::NativeHandles native = device->Native();
        g_instance = static_cast<VkInstance>(native.instance);
        g_physical = static_cast<VkPhysicalDevice>(native.physicalDevice);
        g_device = static_cast<VkDevice>(native.device);
        g_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(native.getInstanceProcAddr);
        g_getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(native.getDeviceProcAddr);
        g_dataDir = dataDir;
        Select(wanted);
    }

    void Select(settings::Upscaler wanted)
    {
        g_active = Available(wanted) ? wanted : settings::Upscaler::Off;
        if (wanted != settings::Upscaler::Off && g_active == settings::Upscaler::Off)
            Log("[gpu] upscaler %s is not available here: the scene is drawn without temporal upscaling", settings::UpscalerName(wanted));
        g_name = "off";
        if (g_active == settings::Upscaler::Dlss)
            g_name = "DLSS " + FileVersion(ExeDirectory() / "nvngx_dlss.dll");
#if CRASH_HAVE_FSR
        else if (g_active == settings::Upscaler::Fsr)
            g_name = "FSR " + g_fsrVersionName;
#endif
        // The one not in use gives its memory back (the GPU is idle: the renderer waited).
#if CRASH_HAVE_DLSS
        if (g_active != settings::Upscaler::Dlss && g_dlss)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss);
            g_dlss = nullptr;
        }
#endif
#if CRASH_HAVE_FSR
        if (g_active != settings::Upscaler::Fsr && g_fsr)
        {
            g_ffx.DestroyContext(&g_fsr, nullptr);
            g_fsr = nullptr;
        }
#endif
    }

    void Shutdown()
    {
#if CRASH_HAVE_DLSS
        if (g_dlss)
            NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss);
        g_dlss = nullptr;
        if (g_ngxParameters)
            NVSDK_NGX_VULKAN_DestroyParameters(g_ngxParameters);
        g_ngxParameters = nullptr;
        if (g_ngxInitialized)
            NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_ngxInitialized = false;
#endif
#if CRASH_HAVE_FSR
        if (g_fsr)
            g_ffx.DestroyContext(&g_fsr, nullptr);
        g_fsr = nullptr;
#endif
    }

    settings::Upscaler Active()
    {
        return g_active;
    }

    const std::string& ActiveName()
    {
        return g_name;
    }

    void Jitter(uint64_t index, uint32_t renderWidth, uint32_t outputWidth, float& x, float& y)
    {
        // 8 * (output / render)^2 phases (the DLSS guide's recommendation; FSR's
        // ffxQueryDescUpscaleGetJitterPhaseCount computes the same).
        float ratio = renderWidth ? float(outputWidth) / float(renderWidth) : 1.0f;
        uint32_t phases = std::max(8u, uint32_t(ceilf(8.0f * ratio * ratio)));
        uint32_t i = uint32_t(index % phases) + 1;
        x = Halton(i, 2) - 0.5f;
        y = Halton(i, 3) - 0.5f;
    }

    bool Evaluate(const Frame& frame)
    {
        bool ok = false;
#if CRASH_HAVE_DLSS
        if (g_active == settings::Upscaler::Dlss)
            ok = EvaluateDlss(frame);
#endif
#if CRASH_HAVE_FSR
        if (g_active == settings::Upscaler::Fsr)
            ok = EvaluateFsr(frame);
#endif
        // The SDKs bound their own pipelines and descriptor sets on the command buffer.
        if (g_rhi)
            g_rhi->InvalidateState();
        return ok;
    }
}
