#include "upscaler.h"
#include "../diagnostics.h"
#include "../settings/gpu_vendor.h"

#include "../platform/text.h"

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include "dlss_module/crash_dlss.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

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
            return plat::ExecutablePath().parent_path();
        }

        // "a.b.c" from a DLL's version resource, or "" if it has none.
        std::string FileVersion(const std::filesystem::path& file)
        {
            return plat::LibraryFileVersion(file);
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

        // DLSS: CrashDlss.dll (dlss_module/crash_dlss.h) ------------------------------------------
        void* g_dlssModule = nullptr;
        struct
        {
            CrashDlss_RequiredExtensionsFn RequiredExtensions;
            CrashDlss_InitFn Init;
            CrashDlss_EvaluateFn Evaluate;
            CrashDlss_ReleaseFeatureFn ReleaseFeature;
            CrashDlss_ShutdownFn Shutdown;
        } g_dlssApi{};
        bool g_ngxInitialized = false;

        void DlssLog(const char* text)
        {
            Log("%s", text);
        }

        // CrashDlss.dll next to the exe, once; false if it isn't there (an install without DLSS: no
        // NVIDIA RTX GPU) or is not the version this exe was built for.
        bool LoadDlssModule()
        {
            static bool tried = false;
            if (tried)
                return g_dlssModule != nullptr;
            tried = true;
            std::filesystem::path dll = ExeDirectory() / "CrashDlss.dll";
            std::error_code error;
            if (!std::filesystem::exists(dll, error))
                return false;
            void* module = plat::OpenLibrary(dll);
            if (!module)
            {
                Log("[gpu] DLSS: CrashDlss.dll could not be loaded (%s)", plat::LastErrorText().c_str());
                return false;
            }
            auto version = reinterpret_cast<CrashDlss_ApiVersionFn>(plat::FindSymbol(module, "CrashDlss_ApiVersion"));
            g_dlssApi.RequiredExtensions = reinterpret_cast<CrashDlss_RequiredExtensionsFn>(plat::FindSymbol(module, "CrashDlss_RequiredExtensions"));
            g_dlssApi.Init = reinterpret_cast<CrashDlss_InitFn>(plat::FindSymbol(module, "CrashDlss_Init"));
            g_dlssApi.Evaluate = reinterpret_cast<CrashDlss_EvaluateFn>(plat::FindSymbol(module, "CrashDlss_Evaluate"));
            g_dlssApi.ReleaseFeature = reinterpret_cast<CrashDlss_ReleaseFeatureFn>(plat::FindSymbol(module, "CrashDlss_ReleaseFeature"));
            g_dlssApi.Shutdown = reinterpret_cast<CrashDlss_ShutdownFn>(plat::FindSymbol(module, "CrashDlss_Shutdown"));
            if (!version || version() != CRASH_DLSS_API_VERSION || !g_dlssApi.RequiredExtensions || !g_dlssApi.Init || !g_dlssApi.Evaluate ||
                !g_dlssApi.ReleaseFeature || !g_dlssApi.Shutdown)
            {
                Log("[gpu] DLSS: CrashDlss.dll is from another version (API %u, this exe wants %u)", version ? version() : 0u, CRASH_DLSS_API_VERSION);
                plat::CloseLibrary(module);
                g_dlssApi = {};
                return false;
            }
            g_dlssModule = module;
            return true;
        }

        // The vendor of the renderer's GPU (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel). CRASH_GPU_VENDOR
        // (a hex id or nvidia / amd / intel) pretends another one, to test the options on one PC.
        uint32_t GpuVendor()
        {
            uint32_t forced = settings::ForcedGpuVendor();
            if (forced)
                return forced;
            auto getProperties =
                g_getInstanceProcAddr ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_getInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties"))
                                      : nullptr;
            if (!getProperties || !g_physical)
                return 0;
            VkPhysicalDeviceProperties properties{};
            getProperties(g_physical, &properties);
            return properties.vendorID;
        }

        void ProbeDlss(const std::wstring& dataDir)
        {
            uint32_t vendor = GpuVendor();
            if (vendor != 0x10DE)
            {
                Log("[gpu] DLSS: needs an NVIDIA RTX GPU (this GPU's vendor is 0x%04X%s)", vendor,
                    settings::ForcedGpuVendor() ? ", CRASH_GPU_VENDOR" : "");
                return;
            }
            std::filesystem::path dll = ExeDirectory() / "nvngx_dlss.dll";
            std::error_code error;
            if (!std::filesystem::exists(dll, error))
            {
                Log("[gpu] DLSS: nvngx_dlss.dll not found next to the exe");
                return;
            }
            if (!LoadDlssModule())
            {
                Log("[gpu] DLSS: CrashDlss.dll not found next to the exe");
                return;
            }
            std::filesystem::create_directories(dataDir, error);
            std::wstring exeDir = ExeDirectory().wstring();
            CrashDlssDevice device{ g_instance, g_physical, g_device, g_getInstanceProcAddr, g_getDeviceProcAddr, dataDir.c_str(), exeDir.c_str(), DlssLog };
            g_ngxInitialized = true;  // Shutdown releases what Init started, even when it failed
            if (!g_dlssApi.Init(&device))
                return;
            g_dlssAvailable = true;
            std::string version = FileVersion(dll);
            Log("[gpu] DLSS available (nvngx_dlss.dll %s)", version.empty() ? "?" : version.c_str());
        }

        CrashDlssImage DlssImage(const NativeTexture& texture)
        {
            return { texture.image, texture.view, texture.format, texture.width, texture.height };
        }

        bool EvaluateDlss(const Frame& frame)
        {
            NativeFrame native = Natives(frame);
            CrashDlssFrame dlss{};
            dlss.commandBuffer = native.commandBuffer;
            dlss.color = DlssImage(native.color);
            dlss.depth = DlssImage(native.depth);
            dlss.motion = DlssImage(native.motion);
            dlss.output = DlssImage(native.output);
            dlss.renderWidth = frame.renderWidth;
            dlss.renderHeight = frame.renderHeight;
            dlss.outputWidth = frame.outputWidth;
            dlss.outputHeight = frame.outputHeight;
            dlss.jitterX = frame.jitterX;
            dlss.jitterY = frame.jitterY;
            dlss.frameTimeMs = frame.frameTimeMs;
            dlss.quality = int32_t(frame.quality);
            dlss.reset = frame.reset ? 1 : 0;
            dlss.depthInverted = frame.depthInverted ? 1 : 0;
            return g_dlssApi.Evaluate(&dlss) != 0;
        }

#if CRASH_HAVE_FSR
        // FidelityFX API ------------------------------------------------------------------------
        const char* const FSR_DEVICE_EXTENSIONS[] = { VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
                                                      VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
                                                      VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME };
        void* g_ffxModule = nullptr;
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
            std::string text = plat::NarrowUtf8(message ? message : L"");
            Log("[fsr] %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text.c_str());
        }

        void ProbeFsr()
        {
            // The DLL calls these through vkGetDeviceProcAddr by their extension names: without them
            // it would call null pointers, so FSR is only offered when the device has them all.
            std::string missing;
            for (const char* name : FSR_DEVICE_EXTENSIONS)
                if (!g_rhi->DeviceExtensionEnabled(name))
                    missing += (missing.empty() ? "" : ", ") + std::string(name);
            if (!missing.empty())
            {
                Log("[gpu] FSR: not available on this GPU (the driver lacks %s)", missing.c_str());
                return;
            }
            std::filesystem::path dll = ExeDirectory() / "amd_fidelityfx_vk.dll";
            g_ffxModule = plat::OpenLibrary(dll);
            if (!g_ffxModule)
            {
                Log("[gpu] FSR: amd_fidelityfx_vk.dll not found next to the exe");
                return;
            }
            g_ffx.CreateContext = reinterpret_cast<PfnFfxCreateContext>(plat::FindSymbol(g_ffxModule, "ffxCreateContext"));
            g_ffx.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(plat::FindSymbol(g_ffxModule, "ffxDestroyContext"));
            g_ffx.Configure = reinterpret_cast<PfnFfxConfigure>(plat::FindSymbol(g_ffxModule, "ffxConfigure"));
            g_ffx.Query = reinterpret_cast<PfnFfxQuery>(plat::FindSymbol(g_ffxModule, "ffxQuery"));
            g_ffx.Dispatch = reinterpret_cast<PfnFfxDispatch>(plat::FindSymbol(g_ffxModule, "ffxDispatch"));
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
        // NGX's list (the same for every NVIDIA GPU; the device enables the ones the driver has).
        uint32_t instanceCount = 0, deviceCount = 0;
        const char* const* instanceNames = nullptr;
        const char* const* deviceNames = nullptr;
        uint32_t forced = settings::ForcedGpuVendor();  // another vendor pretended: as on a PC without DLSS
        if ((!forced || forced == settings::VENDOR_NVIDIA) && LoadDlssModule() &&
            g_dlssApi.RequiredExtensions(&instanceCount, &instanceNames, &deviceCount, &deviceNames))
        {
            for (uint32_t i = 0; i < instanceCount; i++)
                instance.push_back(instanceNames[i]);
            for (uint32_t i = 0; i < deviceCount; i++)
                device.push_back(deviceNames[i]);
        }
#if CRASH_HAVE_FSR
        // amd_fidelityfx_vk.dll looks these entry points up by their extension names
        // (vkGetBufferMemoryRequirements2KHR, vkSetDebugUtilsObjectNameEXT...), which
        // vkGetDeviceProcAddr only resolves with the extension enabled: a null call otherwise.
        instance.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        for (const char* name : FSR_DEVICE_EXTENSIONS)
            device.push_back(name);
#endif
        (void)instance;
        (void)device;
    }

    bool Available(settings::Upscaler upscaler)
    {
        if (!g_device)
            return false;
        int64_t start, end, frequency;
        start = int64_t(plat::PerfCounter());
        bool probed = false;
        if (upscaler == settings::Upscaler::Dlss && !g_dlssProbed)
        {
            g_dlssProbed = probed = true;
            ProbeDlss(g_dataDir);
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
            end = int64_t(plat::PerfCounter());
            frequency = int64_t(plat::PerfFrequency());
            Log("[gpu] %s probed in %.0f ms", settings::UpscalerName(upscaler), double(end - start) * 1000.0 / double(frequency));
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
        // settings.toml asks for DLSS where it can't run (a settings.toml copied from another PC, a
        // new GPU): FSR instead when it can run, else no upscaler. The file is left as it is.
        if (wanted == settings::Upscaler::Dlss && !Available(settings::Upscaler::Dlss))
        {
            settings::Upscaler fallback = Available(settings::Upscaler::Fsr) ? settings::Upscaler::Fsr : settings::Upscaler::Off;
            Log("[gpu] settings.toml asks for DLSS, which this PC can't run (it needs an NVIDIA RTX GPU): using %s instead",
                fallback == settings::Upscaler::Fsr ? "FSR 3.1" : "no upscaler");
            wanted = fallback;
        }
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
        if (g_active != settings::Upscaler::Dlss && g_dlssAvailable)
            g_dlssApi.ReleaseFeature();
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
        if (g_ngxInitialized)
            g_dlssApi.Shutdown();
        g_ngxInitialized = false;
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
        if (g_active == settings::Upscaler::Dlss)
            ok = EvaluateDlss(frame);
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
