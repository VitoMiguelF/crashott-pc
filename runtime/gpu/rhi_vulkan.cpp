// Vulkan backend of the rendering hardware interface (rhi.h). Vulkan 1.3: dynamic rendering,
// synchronization2 and a timeline semaphore for the submissions; VK_KHR_push_descriptor for the
// per-draw textures and the passes' bindings.
//
// - One queue (graphics + present). Each submission signals the next value of the timeline
//   semaphore; the upload ring, the command pools and destroyed objects are reclaimed by value
//   (the D3D12 backend's fence model).
// - Textures track one access state (layout + stages + access); a use in another state queues a
//   barrier, flushed before the next command (outside dynamic rendering: a draw whose textures
//   need barriers ends the rendering instance and begins it again, loading the attachments).
// - Draws: set 0 = push descriptors of the sampled images, set 1 = 4 dynamic uniform buffers in the
//   upload ring (b0-b3) + the ring as a storage buffer (vertex data), set 2 = sampler sets cached by
//   their 32 descriptions (rhi_vulkan_shaders.cpp maps the HLSL registers). Viewport, scissor, blend constants and stencil reference are
//   dynamic; the rest is in pipelines cached by DrawPipelineKey (and a VkPipelineCache on disk).
// - Passes: one regular descriptor set per pass from the command buffer's pool. With push descriptors
//   (and their immutable samplers) NVIDIA's driver sampled zeros from image views created after the
//   first frames: the menus' second front buffer presented black every other frame.
// - The viewport is flipped (negative height) so Direct3D's clip space and window coordinates hold;
//   front faces stay clockwise as in the D3D12 backend.
// - D24S8 depth when the GPU supports it (NVIDIA), D32S8 otherwise (AMD, RADV); the 24-bit depth
//   bias then needs VK_EXT_depth_bias_control (forced unorm representation) when present.
// - Memory: images in 64 MB blocks per memory type (first fit), buffers and big images dedicated.

#include "rhi.h"
#include "rhi_ring.h"
#include "rhi_vulkan_shaders.h"
#include "vulkan_loader.h"
#include "../diagnostics.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace gpu::vk;

namespace gpu::rhi
{
    namespace
    {
        constexpr uint64_t UPLOAD_RING_SIZE = 256ull << 20;
        constexpr uint32_t TIMESTAMPS = 64;
        constexpr uint32_t SAMPLER_SETS = 4096;
        constexpr VkDeviceSize BLOCK_SIZE = 64ull << 20;
        constexpr uint32_t ACQUIRE_SEMAPHORES = 4;
        constexpr uint32_t PASS_SETS = 4096;  // descriptor sets of the passes per command buffer

        const char* ResultName(VkResult result)
        {
            switch (result)
            {
            case VK_SUCCESS: return "VK_SUCCESS";
            case VK_NOT_READY: return "VK_NOT_READY";
            case VK_TIMEOUT: return "VK_TIMEOUT";
            case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
            default: return "VkResult";
            }
        }

        bool Check(VkResult result, const char* what)
        {
            if (result >= VK_SUCCESS)
                return true;
            Log("[vulkan] %s failed: %s (%d)", what, ResultName(result), int(result));
            return false;
        }

        // CRASH_VULKAN_TRACE=N: logs the first N backend calls (finding the call a driver or the
        // validation layers trip on).
        uint32_t g_trace = [] {
            const char* text = getenv("CRASH_VULKAN_TRACE");
            return text ? uint32_t(strtoul(text, nullptr, 10)) : 0u;
        }();
        // CRASH_VULKAN_TRACE_PASSES=<submission>: the passes of 3 submissions from that one.
        uint64_t g_tracePassesFrom = [] {
            const char* text = getenv("CRASH_VULKAN_TRACE_PASSES");
            return text ? uint64_t(strtoull(text, nullptr, 10)) : 0ull;
        }();
#define VK_TRACE(...)                        do                                       {                                            if (g_trace)                             {                                            g_trace--;                               Log("[vulkan] " __VA_ARGS__);         }                                    } while (0)

        // Formats --------------------------------------------------------------------------------

        VkFormat VkFormatOf(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
            case DXGI_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
            case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case DXGI_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
            case DXGI_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
            case DXGI_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
            case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
            case DXGI_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
            case DXGI_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
            case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
            case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
            case DXGI_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
            case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
            case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
            case DXGI_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
            case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
            case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case DXGI_FORMAT_BC1_UNORM_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
            case DXGI_FORMAT_BC2_UNORM_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
            case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
            case DXGI_FORMAT_BC3_UNORM_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
            case DXGI_FORMAT_BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
            case DXGI_FORMAT_BC4_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
            case DXGI_FORMAT_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
            case DXGI_FORMAT_BC5_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
            case DXGI_FORMAT_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
            case DXGI_FORMAT_BC7_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
            case DXGI_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        bool IsDepthFormat(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        }

        // Access states ----------------------------------------------------------------------------

        enum class Access : uint8_t { Undefined, ColorAttachment, DepthAttachment, ShaderRead, TransferSrc, TransferDst, Storage, Present };

        struct AccessInfo
        {
            VkImageLayout layout;
            VkPipelineStageFlags2 stages;
            VkAccessFlags2 access;
            bool writes;
        };

        AccessInfo AccessOf(Access access, bool depth)
        {
            switch (access)
            {
            case Access::ColorAttachment:
                return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true };
            case Access::DepthAttachment:
                return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true };
            case Access::ShaderRead:
                return { depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false };
            case Access::TransferSrc:
                return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false };
            case Access::TransferDst:
                return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true };
            case Access::Storage:
                return { VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true };
            case Access::Present:
                return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, 0, false };
            default:
                return { VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, false };
            }
        }

        // Memory -----------------------------------------------------------------------------------

        struct Allocation
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize offset = 0, size = 0;
            int32_t block = -1;  // -1: dedicated
            uint32_t type = 0;
        };

        struct Block
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint32_t type = 0;
            std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free;  // offset, size; sorted by offset
        };

        class VulkanTexture : public Texture
        {
        public:
            VkImage image = VK_NULL_HANDLE;
            Allocation memory;
            VkFormat format = VK_FORMAT_UNDEFINED;        // sampled view / storage
            VkFormat renderFormat = VK_FORMAT_UNDEFINED;  // attachment view
            VkImageView views[4] = {};                    // View::Default, Depth, Stencil, + attachment view
            bool depth = false;
            bool swapchain = false;
            Access access = Access::Undefined;
            uint64_t lastPass = 0;  // the pass (rendering instance or transfer) of the last write access
        };

        class VulkanBuffer : public Buffer
        {
        public:
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
        };

        class VulkanShader : public Shader
        {
        public:
            VkShaderModule module = VK_NULL_HANDLE;
            Stage stage = Stage::Vertex;
        };

        class VulkanPipeline : public Pipeline
        {
        public:
            VkPipeline pipeline = VK_NULL_HANDLE;
            bool pass = false;
            // Passes: their own layouts.
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            uint32_t textureCount = 0, constantBytes = 0, targetCount = 0;
            bool bufferInput = false;
            std::string name;
            // Draws: the key it was made from, and the last variant for a larger rendering instance.
            DrawPipelineKey key;
            uint64_t variantSignature = 0;
            VulkanPipeline* variant = nullptr;
        };

        class VulkanDevice final : public Device
        {
        public:
            bool Init(const DeviceInfo& info);

            const std::string& AdapterName() const override { return m_adapterName; }

            void Submit() override { SubmitInternal(false); }
            void WaitIdle() override;
            Upload AllocateUpload(uint64_t size, uint64_t alignment) override;

            Texture* CreateTexture(const TextureDesc& desc) override;
            void DestroyTexture(Texture* texture) override;
            Texture* NullTexture() override { return m_nullTexture; }
            Buffer* CreateBuffer(uint64_t size) override { return CreateBuffer(size, false); }
            // `readback`: memory the CPU reads (host cached when the GPU has some).
            Buffer* CreateBuffer(uint64_t size, bool readback);
            void DestroyBuffer(Buffer* buffer) override;

            void CopyUploadToTexture(Texture* texture, uint32_t level, uint64_t offset, uint32_t rowPitch) override;
            void CopyTexture(Texture* destination, Texture* source, const Rect* region) override;
            void ClearColor(Texture* texture, const float color[4], const Rect* rect) override;
            void ClearDepthStencil(Texture* texture, float depth, uint8_t stencil, const Rect* rect) override;

            Shader* CreateShader(Stage stage, const std::string& hlsl, const char* name, Layout layout) override;
            Pipeline* GetDrawPipeline(const DrawPipelineKey& key) override;
            Pipeline* CreatePassPipeline(const PassPipelineDesc& desc) override;

            void Draw(const DrawCall& draw) override;
            void DrawPass(const PassDraw& pass) override;

            DXGI_FORMAT BackBufferFormat() const override { return m_backBufferFormat; }
            void BackBufferSize(uint32_t& width, uint32_t& height) const override
            {
                width = m_swapchainExtent.width;
                height = m_swapchainExtent.height;
            }
            bool Resize(uint32_t width, uint32_t height) override;
            Texture* BeginPresent() override;
            void Present(bool vsync) override;
            void UpdateFullscreen(bool, bool) override {}  // the borderless window is the fullscreen
            void LeaveFullscreen() override {}
            bool ReadBack(Texture* texture, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;

            void WriteTimestamp(uint32_t index) override;
            void ResolveTimestamps(uint32_t, uint32_t) override {}  // read straight from the query pool
            bool ReadTimestamps(uint32_t first, uint32_t count, uint64_t* ticks) override;
            double TimestampFrequency() const override { return m_timestampFrequency; }

            NativeHandles Native() override;
            void* NativeTexture(Texture* texture) override { return texture ? static_cast<VulkanTexture*>(texture)->image : nullptr; }
            void* NativeView(Texture* texture) override { return texture ? ImageView(static_cast<VulkanTexture*>(texture), View::Default) : nullptr; }
            void PrepareNative(Texture* texture, NativeState state) override;
            void InvalidateState() override { m_drawState = {}; }

            Timing TakeTiming() override
            {
                Timing timing = m_timing;
                m_timing = {};
                return timing;
            }
            void LogStats() override;

        private:
            // Startup.
            bool CreateInstance(bool debug);
            bool ChoosePhysicalDevice();
            bool CreateLogicalDevice();
            bool CreateSwapchain(uint32_t width, uint32_t height);
            void DestroySwapchainViews();
            bool CreateDrawLayouts();
            // Submission.
            void SubmitInternal(bool present);
            void BeginCommandBuffer();
            void WaitValue(uint64_t value);
            uint64_t CompletedValue();
            void Collect();
            // Memory and objects.
            int32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags properties);
            // `minimumAlignment`: attachments get their own 64 KB pages (see CreateTexture).
            bool AllocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, Allocation& allocation,
                                VkDeviceSize minimumAlignment = 1);
            void FreeMemory(const Allocation& allocation);
            VkImageView ImageView(VulkanTexture* texture, View view, bool attachment = false);
            VkSampler Sampler(const SamplerDesc& desc);
            VkShaderModule FullscreenVs();
            // Commands.
            void Transition(VulkanTexture* texture, Access access);
            void FlushBarriers();
            void EndRendering();
            void BeginRendering(VulkanTexture* const* colors, uint32_t colorCount, VulkanTexture* depth);
            bool RenderingMatches(VulkanTexture* const* colors, uint32_t colorCount, VulkanTexture* depth) const;
            VulkanPipeline* PipelineInsideRendering(VulkanPipeline* pipeline, VulkanTexture* const* colors, uint32_t colorCount, VulkanTexture* depth);
            void SetViewportScissor(const Viewport& viewport, const Rect& scissor);
            void SavePipelineCache();

            // Device.
            std::string m_adapterName;
            std::filesystem::path m_shaderCache;
            HWND m_hwnd = nullptr;
            bool m_debug = false;
            VkInstance m_instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
            VkSurfaceKHR m_surface = VK_NULL_HANDLE;
            VkPhysicalDevice m_physical = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties m_properties{};
            VkPhysicalDeviceMemoryProperties m_memoryProperties{};
            VkDevice m_device = VK_NULL_HANDLE;
            uint32_t m_queueFamily = 0;
            VkQueue m_queue = VK_NULL_HANDLE;
            bool m_depthClipExtension = false, m_depthBiasControl = false, m_maintenance5 = false;
            std::vector<std::string> m_extraInstanceExtensions, m_extraDeviceExtensions;  // DeviceInfo's
            bool m_depthClamp = false, m_mirrorClampToEdge = false, m_anisotropy = false;
            bool m_d24s8 = false;  // D24_UNORM_S8_UINT can be a sampled depth attachment
            float m_maxAnisotropy = 1.0f;
            double m_timestampFrequency = 0.0;
            uint64_t m_timestampMask = ~0ull;

            // Submission: a timeline semaphore and one command pool per submission in flight.
            VkSemaphore m_timeline = VK_NULL_HANDLE;
            uint64_t m_timelineValue = 0;   // the last value submitted
            uint64_t m_completedValue = 0;  // the last value known complete
            struct Context
            {
                VkCommandPool pool = VK_NULL_HANDLE;
                VkCommandBuffer buffer = VK_NULL_HANDLE;
                VkDescriptorPool descriptors = VK_NULL_HANDLE;  // the passes' sets, reset with the pool
                uint64_t value = 0;
            };
            std::deque<Context> m_pendingContexts;
            std::vector<Context> m_freeContexts;
            Context m_context;
            VkCommandBuffer m_cmd = VK_NULL_HANDLE;
            std::deque<uint64_t> m_frameValues;

            // Swap chain.
            VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
            VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
            DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            VkExtent2D m_swapchainExtent{ 1280, 720 };
            std::vector<std::unique_ptr<VulkanTexture>> m_swapchainImages;
            std::vector<VkSemaphore> m_renderFinished;   // per swap chain image
            VkSemaphore m_acquireSemaphores[ACQUIRE_SEMAPHORES] = {};
            uint64_t m_acquireValues[ACQUIRE_SEMAPHORES] = {};  // the submission that waited on it
            uint32_t m_acquireIndex = 0;
            VkSemaphore m_pendingAcquire = VK_NULL_HANDLE;   // waited by the next submission
            VulkanTexture* m_fallbackBackBuffer = nullptr;    // drawn into when no image could be acquired
            uint32_t m_imageIndex = UINT32_MAX;               // acquired, not presented yet
            bool m_swapchainDirty = false;
            bool m_vsync = false;
            VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
            std::vector<VkPresentModeKHR> m_presentModes;

            // Memory.
            std::vector<Block> m_blocks;
            uint64_t m_dedicatedAllocations = 0;

            // Upload ring.
            VulkanBuffer m_upload;
            Ring m_uploadRing;
            uint64_t m_uploadSize = UPLOAD_RING_SIZE;

            // Draw layouts.
            VkDescriptorSetLayout m_constantsLayout = VK_NULL_HANDLE, m_texturesLayout = VK_NULL_HANDLE, m_samplersLayout = VK_NULL_HANDLE;
            VkPipelineLayout m_drawLayout = VK_NULL_HANDLE;
            VkDescriptorPool m_staticPool = VK_NULL_HANDLE, m_samplerPool = VK_NULL_HANDLE;
            VkDescriptorSet m_constantsSet = VK_NULL_HANDLE;
            uint32_t m_systemRange = 0;
            std::unordered_map<uint64_t, VkDescriptorSet> m_samplerSets;
            std::unordered_map<uint64_t, VkSampler> m_samplers;
            VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
            uint32_t m_unsavedPipelines = 0;
            ULONGLONG m_lastCacheSave = 0;
            std::unordered_map<uint64_t, std::unique_ptr<VulkanPipeline>> m_drawPipelines;
            std::vector<std::unique_ptr<VulkanPipeline>> m_passPipelines;
            std::vector<std::unique_ptr<VulkanShader>> m_shaders;
            VkShaderModule m_fullscreenVs = VK_NULL_HANDLE;
            VulkanTexture* m_nullTexture = nullptr;
            VkQueryPool m_queryPool = VK_NULL_HANDLE;

            // Command state.
            std::vector<VkImageMemoryBarrier2> m_barriers;
            uint64_t m_passId = 1;
            struct Rendering
            {
                bool active = false;
                VulkanTexture* colors[4] = {};
                uint32_t colorCount = 0;
                VulkanTexture* depth = nullptr;
                VkExtent2D extent{};
            } m_rendering;
            // Bindings recorded on the command buffer, so the next draw only records what changed.
            struct DrawState
            {
                bool layoutBound = false;  // set 0 + index buffer bound on this command buffer
                VkPipeline pipeline = VK_NULL_HANDLE;
                VkDescriptorSet samplerSet = VK_NULL_HANDLE;
                bool texturesValid = false;
                uint32_t textureCount = 0;
                TextureView textures[32];
                bool viewportValid = false;
                VkViewport viewport{};
                VkRect2D scissor{};
                bool dynamicValid = false;
                float blendFactor[4] = {};
                uint32_t stencilRef = 0;
                bool effectsValid = false;
                graphics::EffectConstants effects;
                uint64_t effectsOffset = 0;
            } m_drawState;

            // Released once the GPU is past the value.
            struct Garbage
            {
                uint64_t value;
                std::unique_ptr<VulkanTexture> texture;
                std::unique_ptr<VulkanBuffer> buffer;
            };
            std::deque<Garbage> m_garbage;

            Timing m_timing;
            struct
            {
                uint64_t submits = 0, pipelines = 0, shaderCompiles = 0, shaderFailures = 0, renderings = 0, barriers = 0;
                // Why a rendering instance was begun: none open, color count, color targets, depth, pending barriers.
                uint64_t renderingReasons[5] = {};
            } m_stats;
        };

        // Debug messages ----------------------------------------------------------------------------

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            static uint32_t logged;
            if (logged >= 400)
                return VK_FALSE;
            logged++;
            Log("[vulkan] %s: %s", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                data && data->pMessage ? data->pMessage : "?");
            if (logged == 400)
                Log("[vulkan] (no more validation messages are logged)");
            return VK_FALSE;
        }

        // Startup ----------------------------------------------------------------------------------

        bool VulkanDevice::CreateInstance(bool debug)
        {
            uint32_t apiVersion = VK_API_VERSION_1_0;
            if (vkEnumerateInstanceVersion)
                vkEnumerateInstanceVersion(&apiVersion);
            if (apiVersion < VK_API_VERSION_1_3)
            {
                Log("[vulkan] the Vulkan loader is %u.%u; 1.3 is needed", VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion));
                return false;
            }
            uint32_t count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> available(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
            auto has = [&](const char* name) {
                return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
            };
            std::vector<const char*> extensions = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
            std::vector<const char*> layers;
            if (debug)
            {
                uint32_t layerCount = 0;
                vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
                std::vector<VkLayerProperties> layerProperties(layerCount);
                vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data());
                bool validation = std::any_of(layerProperties.begin(), layerProperties.end(),
                                              [](const VkLayerProperties& l) { return strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
                if (validation)
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                else
                    Log("[vulkan] validation layers requested but VK_LAYER_KHRONOS_validation is not installed");
                if (has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
            for (const std::string& name : m_extraInstanceExtensions)
                if (has(name.c_str()) && std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return name == e; }))
                    extensions.push_back(name.c_str());
            VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            app.pApplicationName = "CrashRecomp";
            app.applicationVersion = 1;
            app.pEngineName = "CrashRecomp";
            app.engineVersion = 1;
            app.apiVersion = apiVersion >= VK_API_VERSION_1_4 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;
            VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            create.pApplicationInfo = &app;
            create.enabledExtensionCount = uint32_t(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            create.enabledLayerCount = uint32_t(layers.size());
            create.ppEnabledLayerNames = layers.data();
            if (!Check(vkCreateInstance(&create, nullptr, &m_instance), "vkCreateInstance"))
                return false;
            LoadInstanceFunctions(m_instance);
            m_debug = !layers.empty();
            if (m_debug && vkCreateDebugUtilsMessengerEXT)
            {
                VkDebugUtilsMessengerCreateInfoEXT messenger{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
                messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                messenger.pfnUserCallback = DebugCallback;
                vkCreateDebugUtilsMessengerEXT(m_instance, &messenger, nullptr, &m_messenger);
            }
            VkWin32SurfaceCreateInfoKHR surface{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
            surface.hinstance = GetModuleHandleW(nullptr);
            surface.hwnd = m_hwnd;
            return Check(vkCreateWin32SurfaceKHR(m_instance, &surface, nullptr, &m_surface), "vkCreateWin32SurfaceKHR");
        }

        bool VulkanDevice::ChoosePhysicalDevice()
        {
            uint32_t count = 0;
            vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
            int bestScore = -1;
            for (VkPhysicalDevice device : devices)
            {
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(device, &properties);
                if (properties.apiVersion < VK_API_VERSION_1_3)
                {
                    Log("[vulkan] %s: Vulkan %u.%u, 1.3 is needed", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
                        VK_API_VERSION_MINOR(properties.apiVersion));
                    continue;
                }
                uint32_t extensionCount = 0;
                vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
                std::vector<VkExtensionProperties> extensions(extensionCount);
                vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
                auto has = [&](const char* name) {
                    return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
                };
                if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
                {
                    Log("[vulkan] %s: no swap chain or push descriptors", properties.deviceName);
                    continue;
                }
                uint32_t familyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
                std::vector<VkQueueFamilyProperties> families(familyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
                int family = -1;
                for (uint32_t i = 0; i < familyCount && family < 0; i++)
                {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
                    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && families[i].timestampValidBits)
                        family = int(i);
                }
                if (family < 0)
                    continue;
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                // CRASH_VULKAN_DEVICE=<part of the name> picks a GPU.
                if (const char* wanted = getenv("CRASH_VULKAN_DEVICE"); wanted && strstr(properties.deviceName, wanted))
                    score += 10;
                if (score > bestScore)
                {
                    bestScore = score;
                    m_physical = device;
                    m_properties = properties;
                    m_queueFamily = uint32_t(family);
                    m_depthClipExtension = has(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
                    m_depthBiasControl = has(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
                    m_maintenance5 = has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
                    uint32_t bits = families[family].timestampValidBits;
                    m_timestampMask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
                }
            }
            if (!m_physical)
            {
                Log("[vulkan] no GPU with Vulkan 1.3, a graphics queue that presents to the window and push descriptors");
                return false;
            }
            m_adapterName = m_properties.deviceName;
            vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memoryProperties);
            m_timestampFrequency = m_properties.limits.timestampPeriod > 0.0f ? 1e9 / double(m_properties.limits.timestampPeriod) : 0.0;
            VkFormatProperties d24;
            vkGetPhysicalDeviceFormatProperties(m_physical, VK_FORMAT_D24_UNORM_S8_UINT, &d24);
            m_d24s8 = (d24.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                      (d24.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !getenv("CRASH_VULKAN_D32");
            m_uploadSize = std::min<uint64_t>(UPLOAD_RING_SIZE, m_properties.limits.maxStorageBufferRange & ~uint64_t(0xFFFF));
            return true;
        }

        bool VulkanDevice::CreateLogicalDevice()
        {
            VkPhysicalDeviceVulkan13Features available13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceVulkan12Features available12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &available13 };
            VkPhysicalDeviceVulkan11Features available11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &available12 };
            VkPhysicalDeviceFeatures2 available{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &available11 };
            vkGetPhysicalDeviceFeatures2(m_physical, &available);
            if (!available13.dynamicRendering || !available13.synchronization2 || !available12.timelineSemaphore || !available.features.geometryShader ||
                !available13.shaderDemoteToHelperInvocation)
            {
                Log("[vulkan] %s lacks dynamic rendering, synchronization2, timeline semaphores, demote to helper invocation or geometry shaders",
                    m_adapterName.c_str());
                return false;
            }
            VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            features13.dynamicRendering = VK_TRUE;
            features13.synchronization2 = VK_TRUE;
            // DXC compiles `discard` to OpDemoteToHelperInvocation for Vulkan 1.3 (D3D's semantics).
            features13.shaderDemoteToHelperInvocation = available13.shaderDemoteToHelperInvocation;
            VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &features13 };
            features12.timelineSemaphore = VK_TRUE;
            features12.samplerMirrorClampToEdge = available12.samplerMirrorClampToEdge;
            m_mirrorClampToEdge = available12.samplerMirrorClampToEdge;
            VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12 };
            // What the upscalers' compute shaders use (FidelityFX: fp16, wave size control, typeless
            // storage images; NGX: buffer device addresses), when the GPU has it.
            features13.subgroupSizeControl = available13.subgroupSizeControl;
            features13.computeFullSubgroups = available13.computeFullSubgroups;
            features12.shaderFloat16 = available12.shaderFloat16;
            features12.shaderInt8 = available12.shaderInt8;
            features12.storageBuffer8BitAccess = available12.storageBuffer8BitAccess;
            features12.shaderSubgroupExtendedTypes = available12.shaderSubgroupExtendedTypes;
            features12.bufferDeviceAddress = available12.bufferDeviceAddress;
            features11.storageBuffer16BitAccess = available11.storageBuffer16BitAccess;
            features11.uniformAndStorageBuffer16BitAccess = available11.uniformAndStorageBuffer16BitAccess;
            VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11 };
            features.features.shaderInt16 = available.features.shaderInt16;
            features.features.shaderImageGatherExtended = available.features.shaderImageGatherExtended;
            features.features.shaderStorageImageExtendedFormats = available.features.shaderStorageImageExtendedFormats;
            features.features.shaderStorageImageReadWithoutFormat = available.features.shaderStorageImageReadWithoutFormat;
            features.features.shaderStorageImageWriteWithoutFormat = available.features.shaderStorageImageWriteWithoutFormat;
            features.features.fragmentStoresAndAtomics = available.features.fragmentStoresAndAtomics;
            features.features.geometryShader = VK_TRUE;
            features.features.independentBlend = available.features.independentBlend;
            features.features.samplerAnisotropy = available.features.samplerAnisotropy;
            features.features.depthClamp = available.features.depthClamp;
            features.features.depthBiasClamp = available.features.depthBiasClamp;
            m_anisotropy = available.features.samplerAnisotropy;
            m_depthClamp = available.features.depthClamp;
            m_maxAnisotropy = m_anisotropy ? m_properties.limits.maxSamplerAnisotropy : 1.0f;

            std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
            void** chain = &features13.pNext;
            VkPhysicalDeviceDepthClipEnableFeaturesEXT depthClip{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT };
            if (m_depthClipExtension)
            {
                extensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
                depthClip.depthClipEnable = VK_TRUE;
                *chain = &depthClip;
                chain = &depthClip.pNext;
            }
            VkPhysicalDeviceDepthBiasControlFeaturesEXT biasControl{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT };
            if (m_depthBiasControl)
            {
                VkPhysicalDeviceDepthBiasControlFeaturesEXT query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT };
                VkPhysicalDeviceFeatures2 queryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &query };
                vkGetPhysicalDeviceFeatures2(m_physical, &queryFeatures);
                m_depthBiasControl = query.depthBiasControl && query.leastRepresentableValueForceUnormRepresentation;
                if (m_depthBiasControl)
                {
                    extensions.push_back(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
                    biasControl.depthBiasControl = VK_TRUE;
                    biasControl.leastRepresentableValueForceUnormRepresentation = VK_TRUE;
                    *chain = &biasControl;
                    chain = &biasControl.pNext;
                }
            }
            VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
            if (m_maintenance5)
            {
                // Points: PointSize defaults to 1 when the vertex shader doesn't write it.
                extensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
                maintenance5.maintenance5 = VK_TRUE;
                *chain = &maintenance5;
                chain = &maintenance5.pNext;
            }

            // The vendor SDKs' extensions the driver has (VK_EXT_buffer_device_address is left out: the
            // core feature above replaces it and both together are invalid).
            if (!m_extraDeviceExtensions.empty())
            {
                uint32_t count = 0;
                vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> supported(count);
                vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, supported.data());
                std::string missing;
                for (const std::string& name : m_extraDeviceExtensions)
                {
                    if (name == "VK_EXT_buffer_device_address" || std::any_of(extensions.begin(), extensions.end(), [&](const char* e) { return name == e; }))
                        continue;
                    if (std::any_of(supported.begin(), supported.end(), [&](const VkExtensionProperties& e) { return name == e.extensionName; }))
                        extensions.push_back(name.c_str());
                    else
                        missing += (missing.empty() ? "" : ", ") + name;
                }
                if (!missing.empty())
                    Log("[vulkan] extensions asked for but missing: %s", missing.c_str());
            }

            float priority = 1.0f;
            VkDeviceQueueCreateInfo queue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            queue.queueFamilyIndex = m_queueFamily;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo create{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features };
            create.queueCreateInfoCount = 1;
            create.pQueueCreateInfos = &queue;
            create.enabledExtensionCount = uint32_t(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            if (!Check(vkCreateDevice(m_physical, &create, nullptr, &m_device), "vkCreateDevice"))
                return false;
            LoadDeviceFunctions(m_device);
            vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
            return true;
        }

        bool VulkanDevice::CreateSwapchain(uint32_t width, uint32_t height)
        {
            VkSurfaceCapabilitiesKHR capabilities;
            if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
                return false;
            if (m_presentModes.empty())
            {
                uint32_t count = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &count, nullptr);
                m_presentModes.resize(count);
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &count, m_presentModes.data());
                uint32_t formatCount = 0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &formatCount, nullptr);
                std::vector<VkSurfaceFormatKHR> formats(formatCount);
                vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &formatCount, formats.data());
                // 8-bit UNORM (the present pass writes the game's gamma-encoded colours as they are).
                m_swapchainFormat = VK_FORMAT_UNDEFINED;
                for (const VkSurfaceFormatKHR& format : formats)
                {
                    if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                        continue;
                    if (format.format == VK_FORMAT_B8G8R8A8_UNORM || (format.format == VK_FORMAT_R8G8B8A8_UNORM && m_swapchainFormat == VK_FORMAT_UNDEFINED))
                        m_swapchainFormat = format.format;
                }
                if (m_swapchainFormat == VK_FORMAT_UNDEFINED)
                {
                    Log("[vulkan] the window surface has no 8-bit UNORM format");
                    return false;
                }
                m_backBufferFormat = m_swapchainFormat == VK_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            auto supports = [&](VkPresentModeKHR mode) { return std::find(m_presentModes.begin(), m_presentModes.end(), mode) != m_presentModes.end(); };
            // V-Sync off: immediate (tearing, like the D3D12 swap chain's), else mailbox.
            m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
            if (!m_vsync)
                m_presentMode = supports(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                : supports(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
            VkExtent2D extent = capabilities.currentExtent;
            if (extent.width == UINT32_MAX)
                extent = { width, height };
            extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
            if (!extent.width || !extent.height)
                return false;
            uint32_t imageCount = std::max(capabilities.minImageCount, 3u);
            if (capabilities.maxImageCount)
                imageCount = std::min(imageCount, capabilities.maxImageCount);
            VkSwapchainCreateInfoKHR create{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
            create.surface = m_surface;
            create.minImageCount = imageCount;
            create.imageFormat = m_swapchainFormat;
            create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            create.imageExtent = extent;
            create.imageArrayLayers = 1;
            create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create.preTransform = capabilities.currentTransform;
            create.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                                : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            create.presentMode = m_presentMode;
            create.clipped = VK_TRUE;
            create.oldSwapchain = m_swapchain;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            if (!Check(vkCreateSwapchainKHR(m_device, &create, nullptr, &swapchain), "vkCreateSwapchainKHR"))
                return false;
            DestroySwapchainViews();
            if (m_swapchain)
                vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = swapchain;
            m_swapchainExtent = extent;
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
            std::vector<VkImage> images(count);
            vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, images.data());
            for (VkImage image : images)
            {
                auto texture = std::make_unique<VulkanTexture>();
                texture->desc.width = extent.width;
                texture->desc.height = extent.height;
                texture->desc.format = m_backBufferFormat;
                texture->desc.usage = USAGE_RENDER_TARGET;
                texture->desc.name = "back buffer";
                texture->image = image;
                texture->format = texture->renderFormat = m_swapchainFormat;
                texture->swapchain = true;
                m_swapchainImages.push_back(std::move(texture));
                VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                VkSemaphore finished = VK_NULL_HANDLE;
                vkCreateSemaphore(m_device, &semaphore, nullptr, &finished);
                m_renderFinished.push_back(finished);
            }
            m_imageIndex = UINT32_MAX;
            m_swapchainDirty = false;
            static const char* const MODES[] = { "immediate", "mailbox", "fifo (v-sync)", "fifo relaxed" };
            Log("[vulkan] swap chain %ux%u, %u images, %s, %s", extent.width, extent.height, count,
                m_swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8", m_presentMode <= 3 ? MODES[m_presentMode] : "?");
            return true;
        }

        void VulkanDevice::DestroySwapchainViews()
        {
            for (auto& image : m_swapchainImages)
            {
                for (VkImageView view : image->views)
                    if (view)
                        vkDestroyImageView(m_device, view, nullptr);
            }
            m_swapchainImages.clear();
            for (VkSemaphore semaphore : m_renderFinished)
                vkDestroySemaphore(m_device, semaphore, nullptr);
            m_renderFinished.clear();
        }

        bool VulkanDevice::CreateDrawLayouts()
        {
            // Constants set: b0-b3 (dynamic uniform buffers) and the upload ring (storage buffer, vertex data).
            VkDescriptorSetLayoutBinding constants[5]{};
            for (uint32_t i = 0; i < 5; i++)
            {
                constants[i].binding = i;
                constants[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                constants[i].descriptorCount = 1;
                constants[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layout.bindingCount = 5;
            layout.pBindings = constants;
            if (!Check(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_constantsLayout), "vkCreateDescriptorSetLayout"))
                return false;
            // Textures set: sampled images t0-t31 (push descriptors). Samplers set: s0-s31.
            VkDescriptorSetLayoutBinding textures[32]{}, samplers[32]{};
            for (uint32_t i = 0; i < 32; i++)
            {
                textures[i] = { i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
                samplers[i] = { i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            }
            layout.bindingCount = 32;
            layout.pBindings = textures;
            layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            if (!Check(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_texturesLayout), "vkCreateDescriptorSetLayout"))
                return false;
            layout.pBindings = samplers;
            layout.flags = 0;
            if (!Check(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_samplersLayout), "vkCreateDescriptorSetLayout"))
                return false;
            VkDescriptorSetLayout sets[3];
            sets[vulkan::DRAW_SET_TEXTURES] = m_texturesLayout;
            sets[vulkan::DRAW_SET_CONSTANTS] = m_constantsLayout;
            sets[vulkan::DRAW_SET_SAMPLERS] = m_samplersLayout;
            VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pipelineLayout.setLayoutCount = 3;
            pipelineLayout.pSetLayouts = sets;
            if (!Check(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_drawLayout), "vkCreatePipelineLayout"))
                return false;

            VkDescriptorPoolSize staticSizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
            VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pool.maxSets = 1;
            pool.poolSizeCount = 2;
            pool.pPoolSizes = staticSizes;
            if (!Check(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_staticPool), "vkCreateDescriptorPool"))
                return false;
            VkDescriptorPoolSize samplerSize{ VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLER_SETS * 32 };
            pool.maxSets = SAMPLER_SETS;
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &samplerSize;
            if (!Check(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_samplerPool), "vkCreateDescriptorPool"))
                return false;
            VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocate.descriptorPool = m_staticPool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &m_constantsLayout;
            return Check(vkAllocateDescriptorSets(m_device, &allocate, &m_constantsSet), "vkAllocateDescriptorSets");
        }

        bool VulkanDevice::Init(const DeviceInfo& info)
        {
            m_hwnd = HWND(info.window);
            m_shaderCache = info.shaderCache;
            m_extraInstanceExtensions = info.instanceExtensions;
            m_extraDeviceExtensions = info.deviceExtensions;
            if (!vulkan::CompilerAvailable())
                return false;
            if (!LoadLoader())
            {
                Log("[vulkan] vulkan-1.dll not found (no Vulkan driver installed)");
                return false;
            }
            if (!CreateInstance(info.debug) || !ChoosePhysicalDevice() || !CreateLogicalDevice())
                return false;
            Log("[vulkan] %s, Vulkan %u.%u.%u, driver 0x%08X; depth %s%s%s%s", m_adapterName.c_str(), VK_API_VERSION_MAJOR(m_properties.apiVersion),
                VK_API_VERSION_MINOR(m_properties.apiVersion), VK_API_VERSION_PATCH(m_properties.apiVersion), m_properties.driverVersion,
                m_d24s8 ? "D24S8" : "D32S8 (no sampled D24S8)", m_depthClipExtension ? ", depth clip control" : "",
                m_depthBiasControl ? ", depth bias control" : "", m_debug ? ", validation layers" : "");

            // Timeline semaphore and the first command buffer.
            VkSemaphoreTypeCreateInfo timelineType{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineType };
            if (!Check(vkCreateSemaphore(m_device, &semaphore, nullptr, &m_timeline), "vkCreateSemaphore"))
                return false;
            VkSemaphoreCreateInfo binary{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            for (VkSemaphore& acquire : m_acquireSemaphores)
                vkCreateSemaphore(m_device, &binary, nullptr, &acquire);
            BeginCommandBuffer();

            // Upload ring: host visible, persistently mapped.
            VkBufferCreateInfo buffer{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            buffer.size = m_uploadSize;
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            if (!Check(vkCreateBuffer(m_device, &buffer, nullptr, &m_upload.buffer), "vkCreateBuffer"))
                return false;
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(m_device, m_upload.buffer, &requirements);
            int32_t type = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = uint32_t(type);
            if (type < 0 || !Check(vkAllocateMemory(m_device, &allocate, nullptr, &m_upload.memory), "vkAllocateMemory (upload ring)") ||
                !Check(vkBindBufferMemory(m_device, m_upload.buffer, m_upload.memory, 0), "vkBindBufferMemory") ||
                !Check(vkMapMemory(m_device, m_upload.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m_upload.cpu)), "vkMapMemory"))
                return false;
            m_upload.size = m_uploadSize;
            m_uploadRing.Init(m_uploadSize);

            if (!CreateDrawLayouts())
                return false;

            // Pipeline cache (the driver checks the header: a cache of another GPU or driver is ignored).
            std::vector<char> cacheData;
            if (!m_shaderCache.empty())
            {
                std::ifstream file(m_shaderCache / "vulkan_pipelines.bin", std::ios::binary);
                if (file)
                    cacheData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            VkPipelineCacheCreateInfo cache{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
            cache.initialDataSize = cacheData.size();
            cache.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
            if (vkCreatePipelineCache(m_device, &cache, nullptr, &m_pipelineCache) != VK_SUCCESS)
            {
                cache.initialDataSize = 0;
                cache.pInitialData = nullptr;
                vkCreatePipelineCache(m_device, &cache, nullptr, &m_pipelineCache);
            }

            // A 1x1 transparent black texture for missing textures (D3D12's null SRV reads 0).
            TextureDesc null;
            null.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            null.usage = USAGE_SAMPLED | USAGE_RENDER_TARGET;
            null.name = "null texture";
            m_nullTexture = static_cast<VulkanTexture*>(CreateTexture(null));
            if (!m_nullTexture)
                return false;
            Transition(m_nullTexture, Access::ShaderRead);

            VkQueryPoolCreateInfo queries{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queries.queryCount = TIMESTAMPS;
            if (vkCreateQueryPool(m_device, &queries, nullptr, &m_queryPool) != VK_SUCCESS)
                m_timestampFrequency = 0.0;

            if (!CreateSwapchain(info.width, info.height))
                return false;
            SubmitInternal(false);
            return true;
        }

        // Submission -----------------------------------------------------------------------------

        void VulkanDevice::BeginCommandBuffer()
        {
            Collect();
            if (!m_freeContexts.empty())
            {
                m_context = m_freeContexts.back();
                m_freeContexts.pop_back();
                vkResetCommandPool(m_device, m_context.pool, 0);
                vkResetDescriptorPool(m_device, m_context.descriptors, 0);
            }
            else
            {
                m_context = {};
                VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                pool.queueFamilyIndex = m_queueFamily;
                vkCreateCommandPool(m_device, &pool, nullptr, &m_context.pool);
                VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocate.commandPool = m_context.pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 1;
                vkAllocateCommandBuffers(m_device, &allocate, &m_context.buffer);
                VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, PASS_SETS }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, PASS_SETS * 8 },
                                                  { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, PASS_SETS }, { VK_DESCRIPTOR_TYPE_SAMPLER, PASS_SETS * 2 } };
                VkDescriptorPoolCreateInfo descriptorPool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                descriptorPool.maxSets = PASS_SETS;
                descriptorPool.poolSizeCount = 4;
                descriptorPool.pPoolSizes = sizes;
                Check(vkCreateDescriptorPool(m_device, &descriptorPool, nullptr, &m_context.descriptors), "vkCreateDescriptorPool (passes)");
            }
            m_cmd = m_context.buffer;
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(m_cmd, &begin);
            m_drawState = {};
        }

        uint64_t VulkanDevice::CompletedValue()
        {
            uint64_t value = 0;
            if (vkGetSemaphoreCounterValue(m_device, m_timeline, &value) == VK_SUCCESS)
                m_completedValue = std::max(m_completedValue, value);
            return m_completedValue;
        }

        void VulkanDevice::WaitValue(uint64_t value)
        {
            if (m_completedValue >= value || CompletedValue() >= value)
                return;
            VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            wait.semaphoreCount = 1;
            wait.pSemaphores = &m_timeline;
            wait.pValues = &value;
            VkResult result = vkWaitSemaphores(m_device, &wait, 10'000'000'000ull);
            if (result == VK_TIMEOUT)
                Log("[vulkan] waited 10 s for the GPU (submission %llu): device lost?", (unsigned long long)value);
            else
                Check(result, "vkWaitSemaphores");
            CompletedValue();
        }

        void VulkanDevice::Collect()
        {
            uint64_t completed = CompletedValue();
            m_uploadRing.Retire(completed);
            while (!m_pendingContexts.empty() && m_pendingContexts.front().value <= completed)
            {
                m_freeContexts.push_back(m_pendingContexts.front());
                m_pendingContexts.pop_front();
            }
            while (!m_garbage.empty() && m_garbage.front().value <= completed)
            {
                Garbage& garbage = m_garbage.front();
                if (VulkanTexture* texture = garbage.texture.get())
                {
                    for (VkImageView view : texture->views)
                        if (view)
                            vkDestroyImageView(m_device, view, nullptr);
                    if (texture->image)
                        vkDestroyImage(m_device, texture->image, nullptr);
                    VK_TRACE("  free: block %d offset %llu size %llu (%s, value %llu, completed %llu)", texture->memory.block,
                             (unsigned long long)texture->memory.offset, (unsigned long long)texture->memory.size, texture->desc.name,
                             (unsigned long long)garbage.value, (unsigned long long)completed);
                    FreeMemory(texture->memory);
                }
                if (VulkanBuffer* buffer = garbage.buffer.get())
                {
                    vkDestroyBuffer(m_device, buffer->buffer, nullptr);
                    vkFreeMemory(m_device, buffer->memory, nullptr);
                }
                m_garbage.pop_front();
            }
        }

        void VulkanDevice::SubmitInternal(bool present)
        {
            VK_TRACE("submit%s", present ? " (present)" : "");
            EndRendering();
            FlushBarriers();
            vkEndCommandBuffer(m_cmd);
            m_timelineValue++;
            VkCommandBufferSubmitInfo command{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            command.commandBuffer = m_cmd;
            VkSemaphoreSubmitInfo signals[2]{};
            signals[0] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, m_timeline, m_timelineValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            uint32_t signalCount = 1;
            if (present && m_imageIndex != UINT32_MAX)
                signals[signalCount++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, m_renderFinished[m_imageIndex], 0,
                                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, m_pendingAcquire, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &command;
            submit.signalSemaphoreInfoCount = signalCount;
            submit.pSignalSemaphoreInfos = signals;
            if (m_pendingAcquire)
            {
                submit.waitSemaphoreInfoCount = 1;
                submit.pWaitSemaphoreInfos = &wait;
                m_acquireValues[(m_acquireIndex + ACQUIRE_SEMAPHORES - 1) % ACQUIRE_SEMAPHORES] = m_timelineValue;
                m_pendingAcquire = VK_NULL_HANDLE;
            }
            VkResult result = vkQueueSubmit2(m_queue, 1, &submit, VK_NULL_HANDLE);
            if (!Check(result, "vkQueueSubmit2") && result == VK_ERROR_DEVICE_LOST)
                Fatal("[vulkan] device lost");
            m_uploadRing.OnSubmit(m_timelineValue);
            m_context.value = m_timelineValue;
            m_pendingContexts.push_back(m_context);
            m_stats.submits++;
            BeginCommandBuffer();
        }

        void VulkanDevice::WaitIdle()
        {
            SubmitInternal(false);
            WaitValue(m_timelineValue);
            Collect();
        }

        Device::Upload VulkanDevice::AllocateUpload(uint64_t size, uint64_t alignment)
        {
            uint64_t offset = m_uploadRing.TryAllocate(size, alignment);
            while (offset == UINT64_MAX)
            {
                Collect();
                offset = m_uploadRing.TryAllocate(size, alignment);
                if (offset != UINT64_MAX)
                    break;
                if (size > m_uploadSize)
                    return {};
                LARGE_INTEGER waitStart, waitEnd;
                QueryPerformanceCounter(&waitStart);
                if (m_uploadRing.HasPending())
                    WaitValue(m_uploadRing.OldestPendingFence());
                else
                    WaitIdle();
                QueryPerformanceCounter(&waitEnd);
                m_timing.ringWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
            }
            return { m_upload.cpu + offset, offset };
        }

        // Memory ---------------------------------------------------------------------------------

        int32_t VulkanDevice::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags properties)
        {
            for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; i++)
            {
                if ((bits & (1u << i)) && (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
                    return int32_t(i);
            }
            return -1;
        }

        bool VulkanDevice::AllocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, Allocation& allocation,
                                          VkDeviceSize minimumAlignment)
        {
            int32_t type = FindMemoryType(requirements.memoryTypeBits, properties);
            if (type < 0)
                return false;
            allocation.type = uint32_t(type);
            allocation.size = requirements.size;
            static const bool dedicatedOnly = getenv("CRASH_VULKAN_DEDICATED") != nullptr;  // debug: an allocation per resource
            if (requirements.size <= BLOCK_SIZE / 4 && !dedicatedOnly)
            {
                for (size_t b = 0; b <= m_blocks.size(); b++)
                {
                    if (b == m_blocks.size())
                    {
                        Block block;
                        block.type = uint32_t(type);
                        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                        allocate.allocationSize = BLOCK_SIZE;
                        allocate.memoryTypeIndex = uint32_t(type);
                        if (vkAllocateMemory(m_device, &allocate, nullptr, &block.memory) != VK_SUCCESS)
                            break;
                        block.free.push_back({ 0, BLOCK_SIZE });
                        m_blocks.push_back(std::move(block));
                    }
                    Block& block = m_blocks[b];
                    if (block.type != uint32_t(type))
                        continue;
                    for (size_t i = 0; i < block.free.size(); i++)
                    {
                        auto [offset, size] = block.free[i];
                        VkDeviceSize aligned = AlignUp64(offset, std::max(requirements.alignment, minimumAlignment));
                        if (aligned + requirements.size > offset + size)
                            continue;
                        VkDeviceSize end = aligned + requirements.size;
                        // Split the free range: before the allocation (alignment) and after it.
                        block.free.erase(block.free.begin() + ptrdiff_t(i));
                        if (end < offset + size)
                            block.free.insert(block.free.begin() + ptrdiff_t(i), { end, offset + size - end });
                        if (aligned > offset)
                            block.free.insert(block.free.begin() + ptrdiff_t(i), { offset, aligned - offset });
                        allocation.memory = block.memory;
                        allocation.offset = aligned;
                        allocation.block = int32_t(b);
                        return true;
                    }
                }
            }
            VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = uint32_t(type);
            if (vkAllocateMemory(m_device, &allocate, nullptr, &allocation.memory) != VK_SUCCESS)
                return false;
            allocation.offset = 0;
            allocation.block = -1;
            m_dedicatedAllocations++;
            return true;
        }

        void VulkanDevice::FreeMemory(const Allocation& allocation)
        {
            if (!allocation.memory)
                return;
            if (allocation.block < 0)
            {
                vkFreeMemory(m_device, allocation.memory, nullptr);
                m_dedicatedAllocations--;
                return;
            }
            auto& free = m_blocks[size_t(allocation.block)].free;
            auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(allocation.offset, VkDeviceSize(0)));
            it = free.insert(it, { allocation.offset, allocation.size });
            // Merge with the following and the preceding ranges.
            if (it + 1 != free.end() && it->first + it->second == (it + 1)->first)
            {
                it->second += (it + 1)->second;
                free.erase(it + 1);
            }
            if (it != free.begin() && (it - 1)->first + (it - 1)->second == it->first)
            {
                (it - 1)->second += it->second;
                free.erase(it);
            }
        }

        // Resources ------------------------------------------------------------------------------

        Texture* VulkanDevice::CreateTexture(const TextureDesc& desc)
        {
            VK_TRACE("create texture %ux%u %s", desc.width, desc.height, desc.name);
            auto texture = std::make_unique<VulkanTexture>();
            texture->desc = desc;
            texture->depth = IsDepthFormat(desc.format);
            DXGI_FORMAT format = desc.format;
            if (format == DXGI_FORMAT_D24_UNORM_S8_UINT && !m_d24s8)
                format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            texture->format = VkFormatOf(format);
            texture->renderFormat = texture->depth ? texture->format : VkFormatOf(desc.RenderFormat());
            if (texture->format == VK_FORMAT_UNDEFINED || texture->renderFormat == VK_FORMAT_UNDEFINED)
            {
                Log("[vulkan] no Vulkan format for DXGI %u (%s)", uint32_t(desc.format), desc.name);
                return nullptr;
            }
            VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = texture->format;
            image.extent = { desc.width, desc.height, 1 };
            image.mipLevels = desc.levels;
            image.arrayLayers = 1;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (desc.usage & USAGE_RENDER_TARGET)
                image.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (desc.usage & USAGE_DEPTH_STENCIL)
                image.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if (desc.usage & USAGE_STORAGE)
                image.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            if (texture->renderFormat != texture->format)
                image.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!Check(vkCreateImage(m_device, &image, nullptr, &texture->image), "vkCreateImage"))
            {
                Log("[vulkan] could not create a %ux%u texture (%s, DXGI %u)", desc.width, desc.height, desc.name, uint32_t(desc.format));
                return nullptr;
            }
            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(m_device, texture->image, &requirements);
            // Render targets and depth buffers start on a 64 KB page of their own: packed next to each
            // other at the 1 KB alignment the NVIDIA driver reports, a depth buffer's writes showed up in
            // the color target beside it (the village's scene turned into depth patterns).
            VkDeviceSize minimumAlignment = (desc.usage & (USAGE_RENDER_TARGET | USAGE_DEPTH_STENCIL)) ? 65536 : 1;
            if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture->memory, minimumAlignment) ||
                !Check(vkBindImageMemory(m_device, texture->image, texture->memory.memory, texture->memory.offset), "vkBindImageMemory"))
            {
                Log("[vulkan] out of video memory for a %ux%u texture (%s)", desc.width, desc.height, desc.name);
                vkDestroyImage(m_device, texture->image, nullptr);
                FreeMemory(texture->memory);
                return nullptr;
            }
            VK_TRACE("  memory: block %d offset %llu size %llu alignment %llu type %u", texture->memory.block, (unsigned long long)texture->memory.offset,
                     (unsigned long long)texture->memory.size, (unsigned long long)requirements.alignment, texture->memory.type);
            // Render targets start cleared (color 0, depth 1), like the D3D12 backend's.
            if (desc.usage & (USAGE_RENDER_TARGET | USAGE_DEPTH_STENCIL))
            {
                EndRendering();
                Transition(texture.get(), Access::TransferDst);
                FlushBarriers();
                if (texture->depth)
                {
                    VkClearDepthStencilValue value{ 1.0f, 0 };
                    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
                    vkCmdClearDepthStencilImage(m_cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
                }
                else
                {
                    VkClearColorValue value{};
                    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
                    vkCmdClearColorImage(m_cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
                }
            }
            return texture.release();
        }

        void VulkanDevice::DestroyTexture(Texture* texture)
        {
            if (!texture)
                return;
            auto* vulkanTexture = static_cast<VulkanTexture*>(texture);
            // Not referenced by the bindings of the command buffer any more.
            for (uint32_t i = 0; i < m_rendering.colorCount; i++)
                if (m_rendering.colors[i] == vulkanTexture)
                    EndRendering();
            if (m_rendering.depth == vulkanTexture)
                EndRendering();
            m_drawState.texturesValid = false;
            Garbage garbage{ m_timelineValue + 1 };
            garbage.texture.reset(vulkanTexture);
            m_garbage.push_back(std::move(garbage));
        }

        Buffer* VulkanDevice::CreateBuffer(uint64_t size, bool readback)
        {
            auto buffer = std::make_unique<VulkanBuffer>();
            VkBufferCreateInfo create{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            create.size = size;
            create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (!Check(vkCreateBuffer(m_device, &create, nullptr, &buffer->buffer), "vkCreateBuffer"))
                return nullptr;
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(m_device, buffer->buffer, &requirements);
            int32_t type = -1;
            if (readback)
                type = FindMemoryType(requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (type < 0)
                type = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = uint32_t(type);
            if (type < 0 || vkAllocateMemory(m_device, &allocate, nullptr, &buffer->memory) != VK_SUCCESS)
            {
                vkDestroyBuffer(m_device, buffer->buffer, nullptr);
                Log("[vulkan] could not allocate a %llu-byte buffer", (unsigned long long)size);
                return nullptr;
            }
            vkBindBufferMemory(m_device, buffer->buffer, buffer->memory, 0);
            vkMapMemory(m_device, buffer->memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&buffer->cpu));
            buffer->size = size;
            return buffer.release();
        }

        void VulkanDevice::DestroyBuffer(Buffer* buffer)
        {
            if (!buffer)
                return;
            Garbage garbage{ m_timelineValue + 1 };
            garbage.buffer.reset(static_cast<VulkanBuffer*>(buffer));
            m_garbage.push_back(std::move(garbage));
        }

        VkImageView VulkanDevice::ImageView(VulkanTexture* texture, View view, bool attachment)
        {
            uint32_t slot = attachment ? 3u : uint32_t(view);
            if (texture->views[slot])
                return texture->views[slot];
            VkImageViewCreateInfo create{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            create.image = texture->image;
            create.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create.format = attachment ? texture->renderFormat : texture->format;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            if (texture->depth)
                aspect = attachment ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                    : view == View::Stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
            create.subresourceRange = { aspect, 0, attachment ? 1u : texture->desc.levels, 0, 1 };
            Check(vkCreateImageView(m_device, &create, nullptr, &texture->views[slot]), "vkCreateImageView");
            return texture->views[slot];
        }

        // Barriers and rendering instances -------------------------------------------------------

        void VulkanDevice::Transition(VulkanTexture* texture, Access access)
        {
            AccessInfo after = AccessOf(access, texture->depth);
            if (texture->access == access)
            {
                // Same state: only a write in another pass needs ordering (write after write).
                if (!after.writes || texture->lastPass == m_passId)
                    return;
            }
            // The barriers of one vkCmdPipelineBarrier2 aren't ordered among themselves: a second
            // transition of the same image goes in the next batch.
            for (const VkImageMemoryBarrier2& queued : m_barriers)
            {
                if (queued.image == texture->image)
                {
                    FlushBarriers();
                    break;
                }
            }
            AccessInfo before = AccessOf(texture->access, texture->depth);
            VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = before.stages ? before.stages : VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = before.writes ? before.access : 0;
            barrier.dstStageMask = after.stages ? after.stages : VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = after.access;
            barrier.oldLayout = before.layout;
            barrier.newLayout = after.layout;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = texture->image;
            barrier.subresourceRange = { texture->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                        : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                                         0, VK_REMAINING_MIP_LEVELS, 0, 1 };
            // Present: the swap chain image's contents must be kept (the acquire semaphore orders it).
            if (texture->swapchain && texture->access == Access::Undefined)
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            m_barriers.push_back(barrier);
            texture->access = access;
            texture->lastPass = 0;
        }

        void VulkanDevice::FlushBarriers()
        {
            if (m_barriers.empty())
                return;
            EndRendering();
            VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.imageMemoryBarrierCount = uint32_t(m_barriers.size());
            dependency.pImageMemoryBarriers = m_barriers.data();
            vkCmdPipelineBarrier2(m_cmd, &dependency);
            m_stats.barriers += m_barriers.size();
            m_barriers.clear();
        }

        void VulkanDevice::EndRendering()
        {
            if (!m_rendering.active)
                return;
            vkCmdEndRendering(m_cmd);
            m_rendering.active = false;
        }

        bool VulkanDevice::RenderingMatches(VulkanTexture* const* colors, uint32_t colorCount, VulkanTexture* depth) const
        {
            if (!m_rendering.active || m_rendering.colorCount != colorCount || m_rendering.depth != depth)
                return false;
            for (uint32_t i = 0; i < colorCount; i++)
                if (m_rendering.colors[i] != colors[i])
                    return false;
            return true;
        }

        // A draw whose attachments are a subset of the open rendering instance's (the same image in
        // each slot it writes, the same depth or none) is recorded inside it with a variant of its
        // pipeline made for the instance's attachments: no writes to the extra color slots, no depth
        // or stencil test/write when the draw has no depth. In the scene, draws with and without depth
        // and with one or two color targets (velocity) alternate: ~50 of the ~70 instances per frame
        // in the village were begun only for that.
        VulkanPipeline* VulkanDevice::PipelineInsideRendering(VulkanPipeline* pipeline, VulkanTexture* const* colors, uint32_t colorCount,
                                                                             VulkanTexture* depth)
        {
            if (!m_rendering.active || !m_barriers.empty() || pipeline->pass || colorCount > m_rendering.colorCount)
                return nullptr;
            if (depth && depth != m_rendering.depth)
                return nullptr;
            bool exact = colorCount == m_rendering.colorCount && depth == m_rendering.depth;
            for (uint32_t i = 0; i < colorCount; i++)
            {
                if (colors[i] && colors[i] != m_rendering.colors[i])
                    return nullptr;
                exact = exact && colors[i] == m_rendering.colors[i];
            }
            if (exact)
                return nullptr;  // BeginRendering keeps the instance as it is
            uint64_t signature = uint64_t(m_rendering.colorCount) | (uint64_t(depth != nullptr) << 3) | (uint64_t(m_rendering.depth != nullptr) << 4);
            for (uint32_t i = 0; i < m_rendering.colorCount; i++)
                signature |= uint64_t(m_rendering.colors[i] ? uint32_t(m_rendering.colors[i]->desc.RenderFormat()) & 0xFF : 0) << (8 + 8 * i);
            if (m_rendering.depth)
                signature ^= uint64_t(m_rendering.depth->desc.format) << 48;
            if (pipeline->variant && pipeline->variantSignature == signature)
                return pipeline->variant;
            DrawPipelineKey key = pipeline->key;
            key.keepFormats = 1;
            for (uint32_t i = 0; i < 4; i++)
            {
                VulkanTexture* attachment = i < m_rendering.colorCount ? m_rendering.colors[i] : nullptr;
                key.colorFormats[i] = attachment ? uint32_t(attachment->desc.RenderFormat()) : 0;
                if (i >= colorCount || !colors[i])
                {
                    key.colorMask &= ~(0xFu << (4 * i));
                    key.blend[i] = 0;
                }
            }
            key.colorCount = m_rendering.colorCount;
            if (!depth && m_rendering.depth)
            {
                key.depthFormat = uint32_t(m_rendering.depth->desc.format);
                key.depthControl = 0;
                key.stencilMasks = 0;
                key.depthBias = 0;
                key.slopeBias = 0.0f;
            }
            auto* variant = static_cast<VulkanPipeline*>(GetDrawPipeline(key));
            if (!variant)
                return nullptr;
            pipeline->variant = variant;
            pipeline->variantSignature = signature;
            return variant;
        }

        void VulkanDevice::BeginRendering(VulkanTexture* const* colors, uint32_t colorCount, VulkanTexture* depth)
        {
            if (RenderingMatches(colors, colorCount, depth) && m_barriers.empty())
                return;
            {
                uint32_t reason = 4;
                if (!m_rendering.active)
                    reason = 0;
                else if (m_rendering.colorCount != colorCount)
                    reason = 1;
                else if (m_rendering.depth != depth)
                    reason = 3;
                else
                    for (uint32_t i = 0; i < colorCount; i++)
                        if (m_rendering.colors[i] != colors[i])
                            reason = 2;
                m_stats.renderingReasons[reason]++;
            }
            EndRendering();
            m_passId++;
            VkExtent2D extent{ UINT32_MAX, UINT32_MAX };
            for (uint32_t i = 0; i < colorCount; i++)
            {
                if (!colors[i])
                    continue;
                Transition(colors[i], Access::ColorAttachment);
                extent.width = std::min(extent.width, colors[i]->desc.width);
                extent.height = std::min(extent.height, colors[i]->desc.height);
            }
            if (depth)
            {
                Transition(depth, Access::DepthAttachment);
                extent.width = std::min(extent.width, depth->desc.width);
                extent.height = std::min(extent.height, depth->desc.height);
            }
            FlushBarriers();
            VkRenderingAttachmentInfo colorInfos[4]{};
            for (uint32_t i = 0; i < colorCount; i++)
            {
                colorInfos[i] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                colorInfos[i].imageView = colors[i] ? ImageView(colors[i], View::Default, true) : VK_NULL_HANDLE;
                colorInfos[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorInfos[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colorInfos[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (colors[i])
                    colors[i]->lastPass = m_passId;
            }
            VkRenderingAttachmentInfo depthInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            if (depth)
            {
                depthInfo.imageView = ImageView(depth, View::Default, true);
                depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depth->lastPass = m_passId;
            }
            VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            rendering.renderArea = { { 0, 0 }, extent };
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = colorCount;
            rendering.pColorAttachments = colorInfos;
            rendering.pDepthAttachment = depth ? &depthInfo : nullptr;
            rendering.pStencilAttachment = depth ? &depthInfo : nullptr;
            VK_TRACE("begin rendering %u colors%s, %ux%u", colorCount, depth ? " + depth" : "", extent.width, extent.height);
            vkCmdBeginRendering(m_cmd, &rendering);
            m_rendering.active = true;
            m_rendering.colorCount = colorCount;
            for (uint32_t i = 0; i < 4; i++)
                m_rendering.colors[i] = i < colorCount ? colors[i] : nullptr;
            m_rendering.depth = depth;
            m_rendering.extent = extent;
            m_stats.renderings++;
        }

        void VulkanDevice::SetViewportScissor(const Viewport& viewport, const Rect& scissor)
        {
            // Flipped: Direct3D's clip space and window coordinates (y down) with Vulkan's rasterizer.
            VkViewport vkViewport{ viewport.x, viewport.y + viewport.height, viewport.width, -viewport.height, viewport.minDepth, viewport.maxDepth };
            int32_t left = std::clamp(scissor.left, 0, int32_t(m_rendering.extent.width));
            int32_t top = std::clamp(scissor.top, 0, int32_t(m_rendering.extent.height));
            int32_t right = std::clamp(scissor.right, left, int32_t(m_rendering.extent.width));
            int32_t bottom = std::clamp(scissor.bottom, top, int32_t(m_rendering.extent.height));
            VkRect2D vkScissor{ { left, top }, { uint32_t(right - left), uint32_t(bottom - top) } };
            DrawState& state = m_drawState;
            if (!state.viewportValid || memcmp(&state.viewport, &vkViewport, sizeof(vkViewport)) != 0)
            {
                vkCmdSetViewport(m_cmd, 0, 1, &vkViewport);
                state.viewport = vkViewport;
            }
            if (!state.viewportValid || memcmp(&state.scissor, &vkScissor, sizeof(vkScissor)) != 0)
            {
                vkCmdSetScissor(m_cmd, 0, 1, &vkScissor);
                state.scissor = vkScissor;
            }
            state.viewportValid = true;
        }

        // Copies and clears ----------------------------------------------------------------------

        void VulkanDevice::CopyUploadToTexture(Texture* texture, uint32_t level, uint64_t offset, uint32_t rowPitch)
        {
            VK_TRACE("upload %s level %u", texture->desc.name, level);
            auto* vulkanTexture = static_cast<VulkanTexture*>(texture);
            m_passId++;
            Transition(vulkanTexture, Access::TransferDst);
            FlushBarriers();
            EndRendering();
            uint32_t blockWidth, blockHeight, bytesPerBlock;
            FormatBlock(texture->desc.format, blockWidth, blockHeight, bytesPerBlock);
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.bufferRowLength = rowPitch / bytesPerBlock * blockWidth;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            region.imageExtent = { std::max(texture->desc.width >> level, 1u), std::max(texture->desc.height >> level, 1u), 1 };
            vkCmdCopyBufferToImage(m_cmd, m_upload.buffer, vulkanTexture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vulkanTexture->lastPass = m_passId;
        }

        void VulkanDevice::CopyTexture(Texture* destination, Texture* source, const Rect* region)
        {
            VK_TRACE("copy %s -> %s", source->desc.name, destination->desc.name);
            if (g_tracePassesFrom && m_stats.submits >= g_tracePassesFrom && m_stats.submits < g_tracePassesFrom + 3)
                Log("[vulkan] submit %llu: copy %s %p -> %s %p%s", (unsigned long long)m_stats.submits, source->desc.name, (void*)source,
                    destination->desc.name, (void*)destination, region ? " (region)" : "");
            auto* to = static_cast<VulkanTexture*>(destination);
            auto* from = static_cast<VulkanTexture*>(source);
            m_passId++;
            Transition(from, Access::TransferSrc);
            Transition(to, Access::TransferDst);
            FlushBarriers();
            EndRendering();
            VkImageCopy copy{};
            VkImageAspectFlags aspect = from->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                    : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
            copy.srcSubresource = { aspect, 0, 0, 1 };
            copy.dstSubresource = copy.srcSubresource;
            uint32_t width = std::min(from->desc.width, to->desc.width), height = std::min(from->desc.height, to->desc.height);
            if (region)
            {
                int32_t left = std::max(region->left, 0), top = std::max(region->top, 0);
                int32_t right = std::min(region->right, int32_t(width)), bottom = std::min(region->bottom, int32_t(height));
                if (left >= right || top >= bottom)
                    return;
                copy.srcOffset = copy.dstOffset = { left, top, 0 };
                copy.extent = { uint32_t(right - left), uint32_t(bottom - top), 1 };
            }
            else
            {
                copy.extent = { width, height, 1 };
            }
            vkCmdCopyImage(m_cmd, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            to->lastPass = m_passId;
        }

        void VulkanDevice::ClearColor(Texture* texture, const float color[4], const Rect* rect)
        {
            VK_TRACE("clear color %s", texture->desc.name);
            if (g_tracePassesFrom && m_stats.submits >= g_tracePassesFrom && m_stats.submits < g_tracePassesFrom + 3)
                Log("[vulkan] submit %llu: clear %s %p", (unsigned long long)m_stats.submits, texture->desc.name, (void*)texture);
            auto* vulkanTexture = static_cast<VulkanTexture*>(texture);
            // Inside the current rendering instance when it draws into the texture; else one of its own.
            uint32_t attachment = UINT32_MAX;
            if (m_rendering.active && m_barriers.empty())
            {
                for (uint32_t i = 0; i < m_rendering.colorCount; i++)
                    if (m_rendering.colors[i] == vulkanTexture)
                        attachment = i;
            }
            if (attachment == UINT32_MAX)
            {
                VulkanTexture* colors[1] = { vulkanTexture };
                BeginRendering(colors, 1, nullptr);
                attachment = 0;
            }
            int32_t left = rect ? std::max(rect->left, 0) : 0, top = rect ? std::max(rect->top, 0) : 0;
            int32_t right = rect ? std::min(rect->right, int32_t(m_rendering.extent.width)) : int32_t(m_rendering.extent.width);
            int32_t bottom = rect ? std::min(rect->bottom, int32_t(m_rendering.extent.height)) : int32_t(m_rendering.extent.height);
            if (left >= right || top >= bottom)
                return;
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = attachment;
            memcpy(clear.clearValue.color.float32, color, sizeof(float) * 4);
            VkClearRect clearRect{ { { left, top }, { uint32_t(right - left), uint32_t(bottom - top) } }, 0, 1 };
            vkCmdClearAttachments(m_cmd, 1, &clear, 1, &clearRect);
        }

        void VulkanDevice::ClearDepthStencil(Texture* texture, float depth, uint8_t stencil, const Rect* rect)
        {
            VK_TRACE("clear depth %s", texture->desc.name);
            auto* vulkanTexture = static_cast<VulkanTexture*>(texture);
            if (!(m_rendering.active && m_barriers.empty() && m_rendering.depth == vulkanTexture))
                BeginRendering(nullptr, 0, vulkanTexture);
            int32_t left = rect ? std::max(rect->left, 0) : 0, top = rect ? std::max(rect->top, 0) : 0;
            int32_t right = rect ? std::min(rect->right, int32_t(m_rendering.extent.width)) : int32_t(m_rendering.extent.width);
            int32_t bottom = rect ? std::min(rect->bottom, int32_t(m_rendering.extent.height)) : int32_t(m_rendering.extent.height);
            if (left >= right || top >= bottom)
                return;
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            clear.clearValue.depthStencil = { depth, stencil };
            VkClearRect clearRect{ { { left, top }, { uint32_t(right - left), uint32_t(bottom - top) } }, 0, 1 };
            vkCmdClearAttachments(m_cmd, 1, &clear, 1, &clearRect);
        }

        // Shaders and pipelines ------------------------------------------------------------------

        Shader* VulkanDevice::CreateShader(Stage stage, const std::string& hlsl, const char* name, Layout layout)
        {
            std::vector<uint32_t> spirv;
            if (!vulkan::CompileSpirv(stage, hlsl, name, layout, m_shaderCache, spirv, &m_stats.shaderCompiles, &m_stats.shaderFailures))
                return nullptr;
            auto shader = std::make_unique<VulkanShader>();
            shader->stage = stage;
            VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            create.codeSize = spirv.size() * 4;
            create.pCode = spirv.data();
            if (!Check(vkCreateShaderModule(m_device, &create, nullptr, &shader->module), "vkCreateShaderModule"))
                return nullptr;
            m_shaders.push_back(std::move(shader));
            return m_shaders.back().get();
        }

        VkBlendFactor BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return VK_BLEND_FACTOR_ZERO;
            case 1: return VK_BLEND_FACTOR_ONE;
            case 4: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
            case 5: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
            case 9: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_DST_ALPHA;
            case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case 16: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return VK_BLEND_OP_SUBTRACT;
            case 2: return VK_BLEND_OP_MIN;
            case 3: return VK_BLEND_OP_MAX;
            case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
            default: return VK_BLEND_OP_ADD;
            }
        }

        VkPrimitiveTopology PrimitiveTopology(Topology topology)
        {
            switch (topology)
            {
            case Topology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case Topology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        Pipeline* VulkanDevice::GetDrawPipeline(const DrawPipelineKey& key)
        {
            uint64_t hash = HashBytes(&key, sizeof(key));
            auto it = m_drawPipelines.find(hash);
            if (it != m_drawPipelines.end())
                return it->second->pipeline ? it->second.get() : nullptr;

            VkPipelineShaderStageCreateInfo stages[3]{};
            uint32_t stageCount = 0;
            auto addStage = [&](Shader* shader, VkShaderStageFlagBits stage) {
                stages[stageCount] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                stages[stageCount].stage = stage;
                stages[stageCount].module = static_cast<VulkanShader*>(shader)->module;
                stages[stageCount].pName = "main";
                stageCount++;
            };
            addStage(key.vs, VK_SHADER_STAGE_VERTEX_BIT);
            if (key.gs)
                addStage(key.gs, VK_SHADER_STAGE_GEOMETRY_BIT);
            if (key.ps)
                addStage(key.ps, VK_SHADER_STAGE_FRAGMENT_BIT);

            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            inputAssembly.topology = PrimitiveTopology(key.topology);
            inputAssembly.primitiveRestartEnable = key.stripCut && (key.topology == Topology::TriangleStrip || key.topology == Topology::LineStrip);
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = key.cull == Cull::Front ? VK_CULL_MODE_FRONT_BIT : key.cull == Cull::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_CLOCKWISE;  // as D3D12's FrontCounterClockwise = FALSE (viewport flipped)
            raster.lineWidth = 1.0f;
            raster.depthBiasEnable = key.depthFormat && (key.depthBias != 0 || key.slopeBias != 0.0f);
            raster.depthBiasConstantFactor = float(key.depthBias);
            raster.depthBiasSlopeFactor = key.slopeBias;
            VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClip{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
            VkDepthBiasRepresentationInfoEXT biasRepresentation{ VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT };
            const void** rasterChain = &raster.pNext;
            if (!key.depthClip)
            {
                if (m_depthClipExtension)
                {
                    depthClip.depthClipEnable = VK_FALSE;
                    *rasterChain = &depthClip;
                    rasterChain = &depthClip.pNext;
                }
                else if (m_depthClamp)
                {
                    raster.depthClampEnable = VK_TRUE;
                }
            }
            bool d24 = key.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
            if (raster.depthBiasEnable && d24 && !m_d24s8 && m_depthBiasControl)
            {
                // The 24-bit depth's bias units on the D32 depth buffer that stands in for it.
                biasRepresentation.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
                *rasterChain = &biasRepresentation;
                rasterChain = &biasRepresentation.pNext;
            }

            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            uint32_t depthControl = key.depthControl;
            if (key.depthFormat)
            {
                depthStencil.depthTestEnable = (depthControl >> 1) & 1;
                depthStencil.depthWriteEnable = (depthControl >> 2) & 1;
                depthStencil.depthCompareOp = VkCompareOp((depthControl >> 4) & 7);
                depthStencil.stencilTestEnable = depthControl & 1;
                VkStencilOpState front{};
                front.compareOp = VkCompareOp((depthControl >> 8) & 7);
                front.failOp = VkStencilOp((depthControl >> 11) & 7);
                front.passOp = VkStencilOp((depthControl >> 14) & 7);
                front.depthFailOp = VkStencilOp((depthControl >> 17) & 7);
                front.compareMask = (key.stencilMasks >> 8) & 0xFF;
                front.writeMask = (key.stencilMasks >> 16) & 0xFF;
                VkStencilOpState back = front;
                if ((depthControl >> 7) & 1)
                {
                    back.compareOp = VkCompareOp((depthControl >> 20) & 7);
                    back.failOp = VkStencilOp((depthControl >> 23) & 7);
                    back.passOp = VkStencilOp((depthControl >> 26) & 7);
                    back.depthFailOp = VkStencilOp((depthControl >> 29) & 7);
                }
                depthStencil.front = front;
                depthStencil.back = back;
            }

            VkPipelineColorBlendAttachmentState blends[4]{};
            VkFormat colorFormats[4]{};
            for (uint32_t i = 0; i < key.colorCount; i++)
            {
                uint32_t mask = (key.colorMask >> (4 * i)) & 0xF;
                // A slot the draw doesn't write has no attachment (Draw binds a null view there), unless
                // the draw runs inside a rendering instance with more attachments (keepFormats).
                bool attached = key.keepFormats ? key.colorFormats[i] != 0 : mask != 0;
                colorFormats[i] = attached ? VkFormatOf(DXGI_FORMAT(key.colorFormats[i])) : VK_FORMAT_UNDEFINED;
                VkPipelineColorBlendAttachmentState& blend = blends[i];
                uint32_t control = key.blend[i];
                uint32_t colorSrc = control & 0x1F, colorOp = (control >> 5) & 7, colorDst = (control >> 8) & 0x1F;
                uint32_t alphaSrc = (control >> 16) & 0x1F, alphaOp = (control >> 21) & 7, alphaDst = (control >> 24) & 0x1F;
                blend.blendEnable = !(colorSrc == 1 && colorDst == 0 && colorOp == 0 && alphaSrc == 1 && alphaDst == 0 && alphaOp == 0);
                blend.srcColorBlendFactor = BlendFactor(colorSrc, false);
                blend.dstColorBlendFactor = BlendFactor(colorDst, false);
                blend.colorBlendOp = BlendOp(colorOp);
                blend.srcAlphaBlendFactor = BlendFactor(alphaSrc, true);
                blend.dstAlphaBlendFactor = BlendFactor(alphaDst, true);
                blend.alphaBlendOp = BlendOp(alphaOp);
                blend.colorWriteMask = mask;
            }
            VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            colorBlend.attachmentCount = key.colorCount;
            colorBlend.pAttachments = blends;

            VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                          VK_DYNAMIC_STATE_STENCIL_REFERENCE };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = 4;
            dynamic.pDynamicStates = dynamics;

            VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            rendering.colorAttachmentCount = key.colorCount;
            rendering.pColorAttachmentFormats = colorFormats;
            if (key.depthFormat)
            {
                DXGI_FORMAT depthFormat = DXGI_FORMAT(key.depthFormat);
                if (depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT && !m_d24s8)
                    depthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = VkFormatOf(depthFormat);
            }

            VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
            create.stageCount = stageCount;
            create.pStages = stages;
            create.pVertexInputState = &vertexInput;
            create.pInputAssemblyState = &inputAssembly;
            create.pViewportState = &viewport;
            create.pRasterizationState = &raster;
            create.pMultisampleState = &multisample;
            create.pDepthStencilState = &depthStencil;
            create.pColorBlendState = &colorBlend;
            create.pDynamicState = &dynamic;
            create.layout = m_drawLayout;
            auto pipeline = std::make_unique<VulkanPipeline>();
            pipeline->key = key;
            VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &create, nullptr, &pipeline->pipeline);
            if (result != VK_SUCCESS)
            {
                Log("[vulkan] vkCreateGraphicsPipelines failed (%s) for vs %016llx ps %016llx", ResultName(result), (unsigned long long)key.vsHash,
                    (unsigned long long)key.psHash);
                pipeline->pipeline = VK_NULL_HANDLE;
            }
            m_stats.pipelines++;
            m_unsavedPipelines++;
            Pipeline* out = pipeline->pipeline ? pipeline.get() : nullptr;
            m_drawPipelines[hash] = std::move(pipeline);
            return out;
        }

        VkSampler VulkanDevice::Sampler(const SamplerDesc& desc)
        {
            uint64_t hash = HashBytes(&desc, sizeof(desc));
            auto it = m_samplers.find(hash);
            if (it != m_samplers.end())
                return it->second;
            auto address = [&](Address mode) {
                switch (mode)
                {
                case Address::Wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case Address::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case Address::MirrorOnce: return m_mirrorClampToEdge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case Address::Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                }
            };
            VkSamplerCreateInfo create{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            create.magFilter = desc.magFilter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            create.minFilter = desc.minFilter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            create.mipmapMode = desc.mipFilter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            create.addressModeU = address(desc.addressU);
            create.addressModeV = address(desc.addressV);
            create.addressModeW = address(desc.addressW);
            if (desc.anisotropy > 1 && m_anisotropy)
            {
                create.anisotropyEnable = VK_TRUE;
                create.maxAnisotropy = std::min(float(desc.anisotropy), m_maxAnisotropy);
            }
            create.minLod = desc.minLod;
            create.maxLod = desc.maxLod >= 3.0e38f ? VK_LOD_CLAMP_NONE : desc.maxLod;
            create.borderColor = desc.border == BorderColor::OpaqueWhite ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                 : desc.border == BorderColor::OpaqueBlack ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            VkSampler sampler = VK_NULL_HANDLE;
            Check(vkCreateSampler(m_device, &create, nullptr, &sampler), "vkCreateSampler");
            m_samplers[hash] = sampler;
            return sampler;
        }

        VkShaderModule VulkanDevice::FullscreenVs()
        {
            if (m_fullscreenVs)
                return m_fullscreenVs;
            static const char* FULLSCREEN_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";
            if (Shader* shader = CreateShader(Stage::Vertex, FULLSCREEN_VS, "blit_vs", Layout::Pass))
                m_fullscreenVs = static_cast<VulkanShader*>(shader)->module;
            return m_fullscreenVs;
        }

        Pipeline* VulkanDevice::CreatePassPipeline(const PassPipelineDesc& desc)
        {
            auto pipeline = std::make_unique<VulkanPipeline>();
            pipeline->pass = true;
            pipeline->textureCount = desc.textureCount;
            pipeline->constantBytes = desc.constantBytes;
            pipeline->bufferInput = desc.bufferInput;
            pipeline->targetCount = desc.targetCount;
            pipeline->name = desc.name;

            // b0 (binding 0), t0-t7 (1-8) or the structured buffer (1), immutable s0-s1 (9-10).
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back({ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr });
            if (desc.bufferInput)
                bindings.push_back({ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr });
            else
                for (uint32_t i = 0; i < desc.textureCount; i++)
                    bindings.push_back({ 1 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr });
            VkSampler samplers[2] = {};
            for (uint32_t i = 0; i < desc.samplerCount; i++)
            {
                samplers[i] = Sampler(desc.samplers[i]);
                bindings.push_back({ 9 + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, &samplers[i] });
            }
            VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layout.bindingCount = uint32_t(bindings.size());
            layout.pBindings = bindings.data();
            if (!Check(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &pipeline->setLayout), "vkCreateDescriptorSetLayout"))
                return nullptr;
            VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pipelineLayout.setLayoutCount = 1;
            pipelineLayout.pSetLayouts = &pipeline->setLayout;
            if (!Check(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &pipeline->layout), "vkCreatePipelineLayout"))
                return nullptr;

            VkShaderModule vs = VK_NULL_HANDLE;
            if (desc.vs.empty())
                vs = FullscreenVs();
            else if (Shader* shader = CreateShader(Stage::Vertex, desc.vs, (desc.name + "_vs").c_str(), Layout::Pass))
                vs = static_cast<VulkanShader*>(shader)->module;
            Shader* ps = CreateShader(Stage::Pixel, desc.ps, (desc.name + "_ps").c_str(), Layout::Pass);
            if (!vs || !ps)
                return nullptr;
            VkPipelineShaderStageCreateInfo shaderStages[2]{};
            shaderStages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr };
            shaderStages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                static_cast<VulkanShader*>(ps)->module, "main", nullptr };
            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            VkPipelineColorBlendAttachmentState blends[4]{};
            VkFormat formats[4]{};
            for (uint32_t i = 0; i < desc.targetCount; i++)
            {
                formats[i] = VkFormatOf(desc.targets[i]);
                blends[i].colorWriteMask = 0xF;
                if (desc.blend == Blend::Alpha)
                {
                    blends[i].blendEnable = VK_TRUE;
                    blends[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    blends[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    blends[i].colorBlendOp = VK_BLEND_OP_ADD;
                    blends[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                    blends[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    blends[i].alphaBlendOp = VK_BLEND_OP_ADD;
                }
            }
            VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            colorBlend.attachmentCount = desc.targetCount;
            colorBlend.pAttachments = blends;
            VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dynamics;
            VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            rendering.colorAttachmentCount = desc.targetCount;
            rendering.pColorAttachmentFormats = formats;
            VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
            create.stageCount = 2;
            create.pStages = shaderStages;
            create.pVertexInputState = &vertexInput;
            create.pInputAssemblyState = &inputAssembly;
            create.pViewportState = &viewport;
            create.pRasterizationState = &raster;
            create.pMultisampleState = &multisample;
            create.pDepthStencilState = &depthStencil;
            create.pColorBlendState = &colorBlend;
            create.pDynamicState = &dynamic;
            create.layout = pipeline->layout;
            if (!Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &create, nullptr, &pipeline->pipeline), desc.name.c_str()))
                return nullptr;
            m_stats.pipelines++;
            m_passPipelines.push_back(std::move(pipeline));
            return m_passPipelines.back().get();
        }

        // Drawing --------------------------------------------------------------------------------

        void VulkanDevice::Draw(const DrawCall& draw)
        {
            VK_TRACE("draw %u indices, %u textures, %u targets%s", draw.indexCount, draw.textureCount, draw.colorCount, draw.depthTarget ? " + depth" : "");
            auto* pipeline = static_cast<VulkanPipeline*>(draw.pipeline);
            // Samplers: a set per block of 32 descriptions.
            uint64_t samplerKey = HashBytes(draw.samplers, sizeof(SamplerDesc) * draw.textureCount, 32);
            auto samplerIt = m_samplerSets.find(samplerKey);
            VkDescriptorSet samplerSet;
            if (samplerIt != m_samplerSets.end())
            {
                samplerSet = samplerIt->second;
            }
            else
            {
                if (m_samplerSets.size() >= SAMPLER_SETS)
                {
                    WaitIdle();
                    vkResetDescriptorPool(m_device, m_samplerPool, 0);
                    m_samplerSets.clear();
                }
                VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                allocate.descriptorPool = m_samplerPool;
                allocate.descriptorSetCount = 1;
                allocate.pSetLayouts = &m_samplersLayout;
                if (!Check(vkAllocateDescriptorSets(m_device, &allocate, &samplerSet), "vkAllocateDescriptorSets (samplers)"))
                    return;
                VkDescriptorImageInfo infos[32];
                VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                for (uint32_t i = 0; i < 32; i++)
                    infos[i] = { Sampler(i < draw.textureCount ? draw.samplers[i] : SamplerDesc{}), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
                write.dstSet = samplerSet;
                write.dstBinding = 0;
                write.descriptorCount = 32;
                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                write.pImageInfo = infos;
                vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
                m_samplerSets[samplerKey] = samplerSet;
            }
            // Textures first (their barriers end the rendering instance), then the attachments.
            VulkanTexture* textures[32];
            for (uint32_t i = 0; i < draw.textureCount; i++)
            {
                textures[i] = draw.textures[i].texture ? static_cast<VulkanTexture*>(draw.textures[i].texture) : m_nullTexture;
                Transition(textures[i], Access::ShaderRead);
            }
            VulkanTexture* colors[4];
            for (uint32_t i = 0; i < draw.colorCount; i++)
                colors[i] = static_cast<VulkanTexture*>(draw.colorTargets[i]);
            VulkanTexture* depthTarget = static_cast<VulkanTexture*>(draw.depthTarget);
            if (VulkanPipeline* inside = PipelineInsideRendering(pipeline, colors, draw.colorCount, depthTarget))
                pipeline = inside;
            else
                BeginRendering(colors, draw.colorCount, depthTarget);

            DrawState& state = m_drawState;
            if (!m_systemRange)
            {
                // The constant buffers' sizes are known from the first draw.
                m_systemRange = draw.systemConstantBytes;
                VkDescriptorBufferInfo buffers[5] = { { m_upload.buffer, 0, m_systemRange }, { m_upload.buffer, 0, 4096 }, { m_upload.buffer, 0, 4096 },
                                                      { m_upload.buffer, 0, sizeof(graphics::EffectConstants) }, { m_upload.buffer, 0, VK_WHOLE_SIZE } };
                VkWriteDescriptorSet writes[5]{};
                for (uint32_t i = 0; i < 5; i++)
                {
                    writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[i].dstSet = m_constantsSet;
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo = &buffers[i];
                }
                vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
            }
            if (!state.layoutBound)
            {
                vkCmdBindIndexBuffer(m_cmd, m_upload.buffer, 0, VK_INDEX_TYPE_UINT32);
                state.layoutBound = true;
                state.texturesValid = false;
                state.samplerSet = VK_NULL_HANDLE;
            }
            if (!state.effectsValid || memcmp(&state.effects, &draw.effects, sizeof(draw.effects)) != 0)
            {
                Upload upload = AllocateUpload(sizeof(draw.effects), 256);
                if (!upload.cpu)
                    return;
                memcpy(upload.cpu, &draw.effects, sizeof(draw.effects));
                state.effects = draw.effects;
                state.effectsOffset = upload.offset;
                state.effectsValid = true;
            }
            uint32_t offsets[4] = { uint32_t(draw.systemConstants), uint32_t(draw.vertexConstants), uint32_t(draw.pixelConstants), uint32_t(state.effectsOffset) };
            vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, vulkan::DRAW_SET_CONSTANTS, 1, &m_constantsSet, 4, offsets);

            // Sampled images (push descriptors), when they differ from the last draw's.
            bool sameTextures = state.texturesValid && state.textureCount == draw.textureCount &&
                                memcmp(state.textures, draw.textures, sizeof(TextureView) * draw.textureCount) == 0;
            if (!sameTextures && draw.textureCount)
            {
                VkDescriptorImageInfo images[32];
                VkWriteDescriptorSet writes[32];
                for (uint32_t i = 0; i < draw.textureCount; i++)
                {
                    images[i] = { VK_NULL_HANDLE, ImageView(textures[i], draw.textures[i].texture ? draw.textures[i].view : View::Default),
                                  AccessOf(Access::ShaderRead, textures[i]->depth).layout };
                    writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    writes[i].pImageInfo = &images[i];
                }
                vkCmdPushDescriptorSetKHR(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, vulkan::DRAW_SET_TEXTURES, draw.textureCount, writes);
                memcpy(state.textures, draw.textures, sizeof(TextureView) * draw.textureCount);
                state.textureCount = draw.textureCount;
                state.texturesValid = true;
            }

            if (state.samplerSet != samplerSet)
            {
                vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, vulkan::DRAW_SET_SAMPLERS, 1, &samplerSet, 0, nullptr);
                state.samplerSet = samplerSet;
            }

            if (state.pipeline != pipeline->pipeline)
            {
                vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
                state.pipeline = pipeline->pipeline;
            }
            SetViewportScissor(draw.viewport, draw.scissor);
            if (!state.dynamicValid || memcmp(state.blendFactor, draw.blendFactor, sizeof(draw.blendFactor)) != 0)
            {
                vkCmdSetBlendConstants(m_cmd, draw.blendFactor);
                memcpy(state.blendFactor, draw.blendFactor, sizeof(draw.blendFactor));
            }
            if (!state.dynamicValid || state.stencilRef != draw.stencilRef)
            {
                vkCmdSetStencilReference(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, draw.stencilRef);
                state.stencilRef = draw.stencilRef;
            }
            state.dynamicValid = true;
            vkCmdDrawIndexed(m_cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);
        }

        void VulkanDevice::DrawPass(const PassDraw& pass)
        {
            VK_TRACE("pass %s: %u textures (%s) -> %s", static_cast<VulkanPipeline*>(pass.pipeline)->name.c_str(), pass.textureCount,
                     pass.textureCount && pass.textures[0].texture ? pass.textures[0].texture->desc.name : "-", pass.targets[0] ? pass.targets[0]->desc.name : "?");
            if (g_tracePassesFrom && m_stats.submits >= g_tracePassesFrom && m_stats.submits < g_tracePassesFrom + 3)
                Log("[vulkan] submit %llu: pass %s: %u textures (%s %p) -> %s %p", (unsigned long long)m_stats.submits,
                    static_cast<VulkanPipeline*>(pass.pipeline)->name.c_str(), pass.textureCount,
                    pass.textureCount && pass.textures[0].texture ? pass.textures[0].texture->desc.name : "-",
                    pass.textureCount ? (void*)pass.textures[0].texture : nullptr, pass.targets[0] ? pass.targets[0]->desc.name : "?", (void*)pass.targets[0]);
            auto* pipeline = static_cast<VulkanPipeline*>(pass.pipeline);
            VulkanTexture* textures[8];
            for (uint32_t i = 0; i < pipeline->textureCount; i++)
            {
                textures[i] = i < pass.textureCount && pass.textures[i].texture ? static_cast<VulkanTexture*>(pass.textures[i].texture) : m_nullTexture;
                if (g_tracePassesFrom && m_stats.submits >= g_tracePassesFrom && m_stats.submits < g_tracePassesFrom + 3)
                    Log("[vulkan]   texture %p access %d image %p view %p barriers %zu", (void*)textures[i], int(textures[i]->access),
                        (void*)textures[i]->image, (void*)textures[i]->views[0], m_barriers.size());
                Transition(textures[i], Access::ShaderRead);
            }
            VulkanTexture* targets[4];
            for (uint32_t i = 0; i < pass.targetCount; i++)
                targets[i] = static_cast<VulkanTexture*>(pass.targets[i]);
            Upload constants{};
            if (pass.constantBytes)
            {
                constants = AllocateUpload(AlignUp64(pass.constantBytes, 256), 256);
                if (!constants.cpu)
                    return;
                memcpy(constants.cpu, pass.constants, pass.constantBytes);
            }
            BeginRendering(targets, pass.targetCount, nullptr);

            VkDescriptorBufferInfo constantInfo{ m_upload.buffer, constants.offset, pass.constantBytes ? AlignUp64(pass.constantBytes, 16) : 16 };
            VkDescriptorBufferInfo bufferInfo{};
            VkDescriptorImageInfo images[8];
            VkWriteDescriptorSet writes[9];
            uint32_t writeCount = 0;
            writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[writeCount].dstBinding = 0;
            writes[writeCount].descriptorCount = 1;
            writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[writeCount++].pBufferInfo = &constantInfo;
            if (pipeline->bufferInput)
            {
                auto* buffer = static_cast<VulkanBuffer*>(pass.buffer);
                bufferInfo = { buffer->buffer, pass.bufferOffset, std::max<uint64_t>(pass.bufferBytes, 4) };
                writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[writeCount].dstBinding = 1;
                writes[writeCount].descriptorCount = 1;
                writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[writeCount++].pBufferInfo = &bufferInfo;
            }
            else
            {
                for (uint32_t i = 0; i < pipeline->textureCount; i++)
                {
                    View view = i < pass.textureCount && pass.textures[i].texture ? pass.textures[i].view : View::Default;
                    images[i] = { VK_NULL_HANDLE, ImageView(textures[i], view), AccessOf(Access::ShaderRead, textures[i]->depth).layout };
                    writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[writeCount].dstBinding = 1 + i;
                    writes[writeCount].descriptorCount = 1;
                    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    writes[writeCount++].pImageInfo = &images[i];
                }
            }
            // A set of this command buffer's pool (a new command buffer when it is full).
            VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocate.descriptorPool = m_context.descriptors;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &pipeline->setLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_device, &allocate, &set) != VK_SUCCESS)
            {
                SubmitInternal(false);
                BeginRendering(targets, pass.targetCount, nullptr);
                allocate.descriptorPool = m_context.descriptors;
                if (!Check(vkAllocateDescriptorSets(m_device, &allocate, &set), "vkAllocateDescriptorSets (pass)"))
                    return;
            }
            for (uint32_t i = 0; i < writeCount; i++)
                writes[i].dstSet = set;
            vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);
            vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
            vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 1, &set, 0, nullptr);
            // The draw bindings are disturbed (another layout): the next draw binds everything again.
            m_drawState = {};
            SetViewportScissor(pass.viewport, pass.scissor);
            vkCmdDraw(m_cmd, pass.vertexCount, 1, 0, 0);
            m_drawState.viewportValid = false;
        }

        // Presentation ---------------------------------------------------------------------------

        bool VulkanDevice::Resize(uint32_t width, uint32_t height)
        {
            if (!width || !height)
                return false;
            if (!m_swapchainDirty && width == m_swapchainExtent.width && height == m_swapchainExtent.height)
                return false;
            WaitIdle();
            vkQueueWaitIdle(m_queue);
            return CreateSwapchain(width, height);
        }

        Texture* VulkanDevice::BeginPresent()
        {
            if (m_imageIndex != UINT32_MAX)
                return m_swapchainImages[m_imageIndex].get();
            if (m_swapchainDirty)
                Resize(m_swapchainExtent.width, m_swapchainExtent.height);
            uint32_t slot = m_acquireIndex;
            WaitValue(m_acquireValues[slot]);  // the submission that waited on it is done
            VkSemaphore acquire = m_acquireSemaphores[slot];
            VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, acquire, VK_NULL_HANDLE, &m_imageIndex);
            if (result == VK_ERROR_OUT_OF_DATE_KHR)
            {
                m_swapchainDirty = true;
                Resize(m_swapchainExtent.width, m_swapchainExtent.height);
                result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, acquire, VK_NULL_HANDLE, &m_imageIndex);
            }
            if (result == VK_SUBOPTIMAL_KHR)
                m_swapchainDirty = true;
            else if (result != VK_SUCCESS)
            {
                Check(result, "vkAcquireNextImageKHR");
                m_imageIndex = UINT32_MAX;
                // This frame is drawn into a texture of the back buffer's format and not shown.
                if (!m_fallbackBackBuffer || m_fallbackBackBuffer->desc.width != m_swapchainExtent.width ||
                    m_fallbackBackBuffer->desc.height != m_swapchainExtent.height)
                {
                    DestroyTexture(m_fallbackBackBuffer);
                    TextureDesc desc;
                    desc.width = m_swapchainExtent.width;
                    desc.height = m_swapchainExtent.height;
                    desc.format = m_backBufferFormat;
                    desc.usage = USAGE_RENDER_TARGET;
                    desc.name = "unshown back buffer";
                    m_fallbackBackBuffer = static_cast<VulkanTexture*>(CreateTexture(desc));
                }
                return m_fallbackBackBuffer;
            }
            m_acquireIndex = (m_acquireIndex + 1) % ACQUIRE_SEMAPHORES;
            m_acquireValues[slot] = UINT64_MAX;  // set by the submission that waits on it
            m_pendingAcquire = acquire;
            VulkanTexture* image = m_swapchainImages[m_imageIndex].get();
            image->access = Access::Undefined;  // its old contents aren't needed
            return image;
        }

        void VulkanDevice::Present(bool vsync)
        {
            if (vsync != m_vsync)
            {
                m_vsync = vsync;
                m_swapchainDirty = true;  // another present mode, from the next frame
            }
            LARGE_INTEGER submitStart, submitEnd, presentEnd;
            QueryPerformanceCounter(&submitStart);
            uint32_t imageIndex = m_imageIndex;
            if (imageIndex != UINT32_MAX)
                Transition(m_swapchainImages[imageIndex].get(), Access::Present);
            SubmitInternal(imageIndex != UINT32_MAX);
            QueryPerformanceCounter(&submitEnd);
            if (imageIndex != UINT32_MAX)
            {
                VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores = &m_renderFinished[imageIndex];
                present.swapchainCount = 1;
                present.pSwapchains = &m_swapchain;
                present.pImageIndices = &imageIndex;
                VkResult result = vkQueuePresentKHR(m_queue, &present);
                if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                    m_swapchainDirty = true;
                else
                    Check(result, "vkQueuePresentKHR");
                m_imageIndex = UINT32_MAX;
            }
            QueryPerformanceCounter(&presentEnd);
            m_timing.submitTicks += uint64_t(submitEnd.QuadPart - submitStart.QuadPart);
            m_timing.presentTicks = std::max(m_timing.presentTicks, uint64_t(presentEnd.QuadPart - submitEnd.QuadPart));

            m_frameValues.push_back(m_timelineValue);
            LARGE_INTEGER waitStart, waitEnd;
            QueryPerformanceCounter(&waitStart);
            while (m_frameValues.size() > 2)
            {
                WaitValue(m_frameValues.front());
                m_frameValues.pop_front();
            }
            QueryPerformanceCounter(&waitEnd);
            m_timing.frameWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
            if (m_unsavedPipelines && GetTickCount64() - m_lastCacheSave > 10000)
                SavePipelineCache();
        }

        void VulkanDevice::SavePipelineCache()
        {
            m_lastCacheSave = GetTickCount64();
            m_unsavedPipelines = 0;
            if (m_shaderCache.empty() || !m_pipelineCache)
                return;
            size_t size = 0;
            if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || !size)
                return;
            std::vector<char> data(size);
            if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) != VK_SUCCESS)
                return;
            std::ofstream(m_shaderCache / "vulkan_pipelines.bin", std::ios::binary).write(data.data(), std::streamsize(size));
        }

        bool VulkanDevice::ReadBack(Texture* texture, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
        {
            auto* source = static_cast<VulkanTexture*>(texture);
            width = texture->desc.width;
            height = texture->desc.height;
            Buffer* buffer = CreateBuffer(uint64_t(width) * height * 4, true);
            if (!buffer)
                return false;
            Access before = source->access;
            m_passId++;
            Transition(source, Access::TransferSrc);
            FlushBarriers();
            EndRendering();
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(m_cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(buffer)->buffer, 1, &region);
            // The copy's writes made visible to the host: without it the read could see the buffer's
            // memory before the GPU's caches were written back (screenshots came out black at times).
            VkBufferMemoryBarrier2 hostRead{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
            hostRead.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            hostRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            hostRead.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            hostRead.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostRead.buffer = static_cast<VulkanBuffer*>(buffer)->buffer;
            hostRead.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &hostRead;
            vkCmdPipelineBarrier2(m_cmd, &dependency);
            if (before != Access::Undefined)
                Transition(source, before);
            WaitIdle();
            rgba.assign(buffer->cpu, buffer->cpu + size_t(width) * height * 4);
            DestroyBuffer(buffer);
            if (source->format == VK_FORMAT_B8G8R8A8_UNORM || source->format == VK_FORMAT_B8G8R8A8_SRGB)
            {
                for (size_t i = 0; i < rgba.size(); i += 4)
                    std::swap(rgba[i], rgba[i + 2]);
            }
            return true;
        }

        // Timestamps -------------------------------------------------------------------------------

        void VulkanDevice::WriteTimestamp(uint32_t index)
        {
            if (!m_queryPool || index >= TIMESTAMPS || m_timestampFrequency <= 0.0)
                return;
            FlushBarriers();
            EndRendering();
            vkCmdResetQueryPool(m_cmd, m_queryPool, index, 1);
            vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool, index);
        }

        bool VulkanDevice::ReadTimestamps(uint32_t first, uint32_t count, uint64_t* ticks)
        {
            if (!m_queryPool || first + count > TIMESTAMPS)
                return false;
            if (vkGetQueryPoolResults(m_device, m_queryPool, first, count, sizeof(uint64_t) * count, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                return false;
            for (uint32_t i = 0; i < count; i++)
                ticks[i] &= m_timestampMask;
            return true;
        }

        // Native access ----------------------------------------------------------------------------

        NativeHandles VulkanDevice::Native()
        {
            FlushBarriers();
            EndRendering();
            NativeHandles handles;
            handles.device = m_device;
            handles.queue = m_queue;
            handles.commandList = m_cmd;
            handles.physicalDevice = m_physical;
            handles.instance = m_instance;
            handles.getInstanceProcAddr = reinterpret_cast<void*>(vkGetInstanceProcAddr);
            handles.getDeviceProcAddr = reinterpret_cast<void*>(vkGetDeviceProcAddr);
            return handles;
        }

        void VulkanDevice::PrepareNative(Texture* texture, NativeState state)
        {
            static constexpr Access accesses[] = { Access::ShaderRead, Access::Storage, Access::TransferSrc, Access::TransferDst, Access::ColorAttachment };
            if (!texture)
                return;
            m_passId++;
            Transition(static_cast<VulkanTexture*>(texture), accesses[uint32_t(state)]);
            FlushBarriers();
            EndRendering();
        }

        void VulkanDevice::LogStats()
        {
            Log("[gpu] vulkan: %llu submits, %llu rendering instances, %llu barriers, %llu pipelines, %llu shaders compiled (%llu failed), "
                "%zu memory blocks + %llu dedicated, %zu samplers, %zu sampler sets",
                (unsigned long long)m_stats.submits, (unsigned long long)m_stats.renderings, (unsigned long long)m_stats.barriers,
                (unsigned long long)m_stats.pipelines, (unsigned long long)m_stats.shaderCompiles, (unsigned long long)m_stats.shaderFailures,
                m_blocks.size(), (unsigned long long)m_dedicatedAllocations, m_samplers.size(), m_samplerSets.size());
            const uint64_t* r = m_stats.renderingReasons;
            Log("[gpu] vulkan: rendering instances begun because none was open %llu, color count %llu, color targets %llu, depth %llu, barriers %llu",
                (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4]);
        }
    }

    Device* CreateVulkan(const DeviceInfo& info)
    {
        auto device = std::make_unique<VulkanDevice>();
        if (!device->Init(info))
            return nullptr;  // (the half-made device is leaked: the renderer falls back to D3D12 once)
        return device.release();
    }
}
