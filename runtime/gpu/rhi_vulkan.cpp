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
//   their 32 descriptions (rhi_vulkan_shaders.cpp maps the HLSL registers). Viewport, scissor, blend constants, stencil reference,
//   cull mode, depth/stencil tests and ops, depth bias, topology and primitive restart are dynamic; the rest is in compiled
//   pipelines (Program, by ProgramKey) shared by the draws' DrawPipelineKeys, a VkPipelineCache on disk, and records of the
//   pipelines seen (pipeline_keys.bin) that compile workers build again at start (see "Compiled draw pipelines").
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
#include "../platform/platform.h"
#include "window.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
        // Messages the validation layers sent (LogStats).
        uint64_t g_validationMessages = 0;
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
            uint64_t hash = 0;  // vulkan::SpirvHash: the .spv in the cache, and the identity in a ProgramKey
        };

        // Compiled draw pipelines --------------------------------------------------------------------
        //
        // What a draw pipeline is compiled from: the shaders (by SpirvHash) and the state that isn't
        // dynamic. Many DrawPipelineKeys share one: cull mode, depth/stencil tests and ops, depth bias,
        // the topology within its class and primitive restart are set per draw (DrawDynamic).
        // Hashed and written to pipeline_keys.bin as bytes: no implicit padding.
        struct ProgramKey
        {
            uint64_t vs = 0, gs = 0, ps = 0;  // ps 0: depth only
            uint32_t colorFormats[4] = {};    // DXGI; 0 = no attachment in that slot
            uint32_t depthFormat = 0;         // DXGI as created (D32S8 where D24S8 can't be sampled); 0 = none
            uint32_t colorCount = 0;
            uint32_t blend[4] = {};           // RB_BLENDCONTROL; opaque where the slot isn't written
            uint32_t colorMask = 0;
            uint8_t topologyClass = 0;        // 0 points, 1 lines, 2 triangles
            uint8_t depthClip = 1;
            uint8_t library = 0;              // library keys (LibraryKey): 1 + the part's bit index
            uint8_t padding[9] = {};
        };
        static_assert(sizeof(ProgramKey) == 80);

        // The dynamic state of a draw (from its DrawPipelineKey), recorded when it differs from the
        // last draw's.
        struct DrawDynamic
        {
            uint8_t cull = VK_CULL_MODE_NONE;
            uint8_t topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            uint8_t restart = 0;
            uint8_t depthTest = 0, depthWrite = 0, depthCompare = 0, stencilTest = 0, biasEnable = 0;
            uint8_t biasUnorm = 0;  // the 24-bit depth's bias units on the D32 depth that stands in for it
            uint8_t padding[3] = {};
            uint32_t frontOps = 0, backOps = 0;  // compare | fail << 8 | pass << 16 | depth fail << 24
            uint32_t compareMask = 0, writeMask = 0;
            float biasConstant = 0.0f, biasSlope = 0.0f;
        };
        static_assert(sizeof(DrawDynamic) == 36);

        // A pipeline (or pipeline library) built once, by the recording thread or a compile worker,
        // whichever gets to it first; the others wait for it (VulkanDevice::Build).
        struct Lazy
        {
            enum : uint8_t { IDLE, QUEUED, BUILDING, BUILT };
            std::atomic<uint8_t> state{ IDLE };
            VkPipeline handle = VK_NULL_HANDLE;  // valid once BUILT (null: it failed)
            bool Built() const { return state.load(std::memory_order_acquire) == BUILT; }
        };

        struct Program
        {
            ProgramKey key;
            // Monolithic, or linked from the libraries with link-time optimization.
            Lazy full;
            // Graphics pipeline libraries: fast-linked, drawn with until `full` is built.
            Lazy fast;
            VkPipeline Current() const
            {
                if (full.Built() && full.handle)
                    return full.handle;
                return fast.Built() ? fast.handle : VK_NULL_HANDLE;
            }
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
            // Draws: the key it was made from, its compiled pipeline and dynamic state, and the last
            // variant for a larger rendering instance.
            DrawPipelineKey key;
            Program* program = nullptr;
            bool failed = false;  // the program didn't compile (logged)
            DrawDynamic dynamic;
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
            Pipeline* GetDrawPipeline(const DrawPipelineKey& key, bool deferrable, bool* deferred) override;
            void PrecompileProgress(uint32_t& done, uint32_t& total) override
            {
                done = m_precompileDone.load();
                total = m_precompileTotal.load();
            }
            void SetShaderCompilation(ShaderCompilation mode) override
            {
                if (mode == m_compileMode)
                    return;
                m_compileMode = mode;
                m_gpl = m_gplAvailable && mode != ShaderCompilation::Sync;
                if (mode != ShaderCompilation::Sync && !m_workerCount)
                    StartCompileWorkers();
                Log("[vulkan] shader compilation: %s", mode == ShaderCompilation::Sync ? "sync" : mode == ShaderCompilation::Aggressive ? "aggressive" : "async");
            }
            void PrewarmShader(Stage stage, const std::string& hlsl) override;
            void PrecompileDone()
            {
                // Once: the warm-up keeps adding parts, so the queue can drain more than once.
                if (++m_precompileDone == m_precompileTotal && !m_precompileLogged.exchange(true))
                    Log("[vulkan] start-up compiles done: %u (pipelines of earlier runs and parts of known shaders) in %llu ms", m_precompileTotal.load(),
                        (unsigned long long)(plat::TickCountMs() - m_precompileStart));
            }
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
            void Probe(Texture* texture, const std::string& tag) override;
            void CollectProbes(uint64_t completed);
            bool Profiling() const override { return m_profiling; }
            void ProfileLabel(const char* label) override { m_profileLabel = label; }
            void ProfileStamp(const char* operation, const VulkanTexture* target);
            void ProfileEnd();
            uint32_t ProfileQuery();
            void CollectProfile(uint64_t completed);
            void LogProfile();

            void WriteTimestamp(uint32_t index) override;
            void ResolveTimestamps(uint32_t, uint32_t) override {}  // read straight from the query pool
            bool ReadTimestamps(uint32_t first, uint32_t count, uint64_t* ticks) override;
            double TimestampFrequency() const override { return m_timestampFrequency; }

            NativeHandles Native() override;
            bool DeviceExtensionEnabled(const char* name) const override
            {
                return std::any_of(m_enabledDeviceExtensions.begin(), m_enabledDeviceExtensions.end(), [&](const std::string& e) { return e == name; });
            }
            void* NativeTexture(Texture* texture) override { return texture ? static_cast<VulkanTexture*>(texture)->image : nullptr; }
            void* NativeView(Texture* texture) override { return texture ? ImageView(static_cast<VulkanTexture*>(texture), View::Default) : nullptr; }
            void PrepareNative(Texture* texture, NativeState state) override;
            void InvalidateState() override { m_drawState = {}; }
            void HoldUploads(bool hold) override { hold ? m_uploadRing.Hold() : m_uploadRing.Release(); }

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
            void NoteSuboptimal(const char* call);
            void LogDiagnostics();
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
            void SetDrawDynamic(const DrawDynamic& dynamic);
            void SavePipelineCache();
            // Draw pipelines (see "Compiled draw pipelines").
            ProgramKey MakeProgramKey(const DrawPipelineKey& key) const;
            DrawDynamic MakeDrawDynamic(const DrawPipelineKey& key) const;
            Program* FindProgram(const ProgramKey& key, bool record);
            Lazy* FindLibrary(const ProgramKey& key, uint32_t part);
            VulkanShader* ShaderByHash(uint64_t hash);
            VulkanShader* LoadCachedSpirv(uint64_t hash, Stage stage);
            // `parts`: VK_GRAPHICS_PIPELINE_LIBRARY_*_BIT_EXT (all four, ALL_PARTS: a whole pipeline);
            // or links `libraries` (the four parts) with or without link-time optimization.
            VkPipeline CreateGraphics(const ProgramKey& key, uint32_t parts, const VkPipeline* libraries = nullptr, bool optimize = false);
            bool Libraries(const ProgramKey& key, Lazy* libraries[4]);
            VkPipeline Build(Lazy& lazy, const std::function<VkPipeline()>& build);
            void Queue(Lazy& lazy, bool urgent, std::function<VkPipeline()> build);
            bool PrepareProgram(Program& program, bool deferrable, bool& deferred);
            void StartCompileWorkers();
            void Enqueue(bool urgent, std::function<void()> job);
            void StartPrecompile();

            // Device.
            std::string m_adapterName;
            std::filesystem::path m_shaderCache;
            bool m_debug = false;
            uint32_t m_safe = 0;  // SafeMode bits (rhi.h)
            bool m_diagnostics = false;
            VkInstance m_instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
            VkSurfaceKHR m_surface = VK_NULL_HANDLE;
            VkPhysicalDevice m_physical = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties m_properties{};
            VkPhysicalDeviceMemoryProperties m_memoryProperties{};
            VkDevice m_device = VK_NULL_HANDLE;
            uint32_t m_queueFamily = 0;
            VkQueue m_queue = VK_NULL_HANDLE;
            bool m_depthClipExtension = false, m_depthBiasControl = false, m_maintenance5 = false, m_memoryBudget = false;
            bool m_gplExtension = false;  // VK_EXT_graphics_pipeline_library + VK_KHR_pipeline_library listed
            std::vector<std::string> m_enabledDeviceExtensions;
            std::unordered_map<uint32_t, VkFormatFeatureFlags> m_formatFeatures;  // (format, usage) pairs checked by CreateTexture
            std::vector<std::string> m_extraInstanceExtensions, m_extraDeviceExtensions;  // DeviceInfo's
            bool m_depthClamp = false, m_mirrorClampToEdge = false, m_anisotropy = false;
            bool m_d24s8 = false;  // D24_UNORM_S8_UINT can be a sampled depth attachment
            float m_maxAnisotropy = 1.0f;
            double m_timestampFrequency = 0.0;
            uint64_t m_timestampMask = ~0ull;

            // Submission: a timeline semaphore and one command pool per submission in flight.
            VkSemaphore m_timeline = VK_NULL_HANDLE;
            // CRASH_VULKAN_GPU_LOAD=<k> (debug): a pass of k thousand dependent iterations per pixel
            // of a 64x64 target in every frame, so this GPU runs behind the CPU like a slow one (an
            // integrated GPU at 1080p): frames in flight, the upload ring's reuse and the swap chain
            // then see the timing they get there.
            uint32_t m_gpuLoad = 0;
            VulkanPipeline* m_gpuLoadPipeline = nullptr;
            VulkanTexture* m_gpuLoadTarget = nullptr;
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
            uint32_t m_suboptimalStreak = 0;  // frames in a row with VK_SUBOPTIMAL_KHR (NoteSuboptimal)
            bool m_frameSuboptimal = false;
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
            std::atomic<uint32_t> m_unsavedPipelines{ 0 };
            uint64_t m_lastCacheSave = 0;
            std::unordered_map<uint64_t, std::unique_ptr<VulkanPipeline>> m_drawPipelines;  // by DrawPipelineKey (recording thread)
            std::vector<std::unique_ptr<VulkanPipeline>> m_passPipelines;
            std::vector<std::unique_ptr<VulkanShader>> m_shaders;

            // Compiled draw pipelines. The compile workers add programs, libraries and shaders too:
            // m_programMutex guards these maps and m_shaders (an object stays where it is once added).
            ShaderCompilation m_compileMode = ShaderCompilation::Async;
            bool m_gplAvailable = false;     // graphics pipeline libraries with fast linking enabled
            std::atomic<bool> m_gpl{ false };  // ... and in use (the asynchronous modes)
            std::mutex m_programMutex;
            std::unordered_map<uint64_t, std::unique_ptr<Program>> m_programs;   // by ProgramKey
            std::unordered_map<uint64_t, std::unique_ptr<Lazy>> m_libraries;     // by LibraryKey
            std::unordered_map<uint64_t, VulkanShader*> m_shaderByHash;
            std::unordered_set<uint64_t> m_recorded;   // ProgramKeys in pipeline_keys.bin
            std::vector<ProgramKey> m_newRecords;      // to append to it
            std::mutex m_lazyMutex;
            std::condition_variable m_lazyBuilt;
            // Compile workers: urgent jobs (pipelines being drawn with) before the start-up ones.
            std::mutex m_jobMutex;
            std::condition_variable m_jobReady;
            std::deque<std::function<void()>> m_jobs[2];  // [0] urgent, [1] start-up
            uint32_t m_workerCount = 0;
            std::atomic<uint32_t> m_precompileDone{ 0 }, m_precompileTotal{ 0 };
            uint64_t m_precompileStart = plat::TickCountMs();
            std::atomic<bool> m_precompileLogged{ false };
            struct
            {
                std::atomic<uint64_t> programs{ 0 }, libraries{ 0 }, fastLinks{ 0 }, optimized{ 0 }, failures{ 0 };
                std::atomic<uint64_t> deferred{ 0 }, stalls{ 0 };
            } m_compileStats;
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
                bool drawDynamicValid = false;
                DrawDynamic drawDynamic;
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

            // Probe: slots of 64 texels in a readback buffer, read in order once their submission is done.
            static constexpr uint32_t PROBE_SLOTS = 4096, PROBE_TEXELS = 64;
            struct ProbeRecord
            {
                uint64_t value;
                uint32_t slot;
                bool bgra;
                std::string tag;
            };
            Buffer* m_probeBuffer = nullptr;
            uint32_t m_probeNext = 0;
            std::deque<ProbeRecord> m_probes;

            // Profile (CRASH_GPU_PROFILE): a timestamp where each rendering instance, pass or copy of
            // the profiled frames begins and at the end of each command buffer, in a ring of queries
            // read in order once their submission is done.
            static constexpr uint32_t PROFILE_QUERIES = 16384, PROFILE_BLOCK = 1024;
            static constexpr uint32_t PROFILE_END = UINT32_MAX;
            struct ProfileRecord
            {
                uint64_t value;
                uint32_t query;
                uint32_t entry;     // into m_profileEntries; PROFILE_END: end of the command buffer
                uint64_t barriers;  // m_stats.barriers when written
            };
            struct ProfileEntry
            {
                std::string name;
                uint64_t ticks = 0, count = 0, barriers = 0;
            };
            VkQueryPool m_profilePool = VK_NULL_HANDLE;
            uint64_t m_profileFirst = 1, m_profileLast = 0, m_presents = 0;
            bool m_profiling = false, m_profileLogged = false;
            bool m_profileDraws = false;  // CRASH_GPU_PROFILE_DRAWS: a timestamp before each draw too
            uint64_t m_profileNext = 0, m_profileReset = 0;  // queries used / reset so far (the ring's index mod PROFILE_QUERIES)
            std::string m_profileLabel;
            const char* m_profileOperation = nullptr;  // the pass or clear beginning a rendering instance
            std::deque<ProfileRecord> m_profileRecords;
            std::unordered_map<std::string, uint32_t> m_profileIndex;
            std::vector<ProfileEntry> m_profileEntries;

            Timing m_timing;
            struct
            {
                uint64_t submits = 0, pipelines = 0, shaderCompiles = 0, shaderFailures = 0, renderings = 0, barriers = 0;
                uint64_t swapchains = 0, suboptimal = 0, unshownFrames = 0;
                uint64_t ringWaits = 0, ringSubmits = 0, ringPeak = 0;  // upload ring: waits for room, of them submits mid-frame, most bytes in use
                // Why a rendering instance was begun: none open, color count, color targets, depth, pending barriers.
                uint64_t renderingReasons[5] = {};
            } m_stats;
        };

        // Debug messages ----------------------------------------------------------------------------

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            // Each message (by its id) is logged 3 times at most, and 1000 in all: a hazard repeated
            // every frame doesn't hide the other ones.
            static std::mutex mutex;
            static std::unordered_map<int32_t, uint32_t> perId;
            static uint32_t logged;
            std::lock_guard<std::mutex> lock(mutex);
            g_validationMessages++;
            int32_t id = data ? data->messageIdNumber : 0;
            uint32_t& count = perId[id];
            count++;
            if (count > 3 || logged >= 1000)
                return VK_FALSE;
            logged++;
            Log("[vulkan] %s: %s", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                data && data->pMessage ? data->pMessage : "?");
            if (logged == 1000)
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
            if (debug)
            {
                // VK_EXT_debug_utils may come from the validation layer only (not the loader or the
                // drivers): without it no messenger is made and the layer's messages never reach the log.
                uint32_t layerExtensions = 0;
                if (vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerExtensions, nullptr) == VK_SUCCESS && layerExtensions)
                {
                    size_t start = available.size();
                    available.resize(start + layerExtensions);
                    vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerExtensions, available.data() + start);
                    available.resize(start + layerExtensions);
                }
            }
            std::vector<const char*> extensions = window::VulkanSurfaceExtensions();
            std::vector<const char*> layers;
            // Validation: core checks + synchronization validation (hazards between passes, copies,
            // clears and the swap chain); CRASH_GPU_DEBUG=nosync leaves synchronization out (faster).
            static const VkBool32 enabled = VK_TRUE;
            VkLayerSettingEXT layerSettings[1] = { { "VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled } };
            VkLayerSettingsCreateInfoEXT settingsInfo{ VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT };
            bool syncValidation = false;
            if (debug)
            {
                uint32_t layerCount = 0;
                vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
                std::vector<VkLayerProperties> layerProperties(layerCount);
                vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data());
                bool validation = std::any_of(layerProperties.begin(), layerProperties.end(),
                                              [](const VkLayerProperties& l) { return strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
                if (validation)
                {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    const char* mode = getenv("CRASH_GPU_DEBUG");
                    syncValidation = !(mode && strstr(mode, "nosync"));
                    if (syncValidation && has(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME))
                    {
                        extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
                        settingsInfo.settingCount = 1;
                        settingsInfo.pSettings = layerSettings;
                    }
                    else
                        syncValidation = false;
                }
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
            VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, settingsInfo.settingCount ? &settingsInfo : nullptr };
            create.pApplicationInfo = &app;
            create.enabledExtensionCount = uint32_t(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            create.enabledLayerCount = uint32_t(layers.size());
            create.ppEnabledLayerNames = layers.data();
            if (!Check(vkCreateInstance(&create, nullptr, &m_instance), "vkCreateInstance"))
                return false;
            LoadInstanceFunctions(m_instance);
            m_debug = !layers.empty();
            if (m_debug)
                Log("[vulkan] validation layers on%s", syncValidation ? ", with synchronization validation" : "");
            if (m_debug && vkCreateDebugUtilsMessengerEXT)
            {
                VkDebugUtilsMessengerCreateInfoEXT messenger{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
                messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                messenger.pfnUserCallback = DebugCallback;
                vkCreateDebugUtilsMessengerEXT(m_instance, &messenger, nullptr, &m_messenger);
            }
            if (m_debug && !m_messenger)
                Log("[vulkan] no debug messenger (VK_EXT_debug_utils missing): the validation messages go to the layer's own output");
            if (!window::CreateVulkanSurface(m_instance, &m_surface))
            {
                Log("[vulkan] could not create the window's surface");
                return false;
            }
            return true;
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
                if (devices.size() > 1)
                    Log("[vulkan] GPU candidate: %s (%s, Vulkan %u.%u)", properties.deviceName, score == 3 ? "discrete" : score == 2 ? "integrated" : "other",
                        VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
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
                    m_memoryBudget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                    m_gplExtension = has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
                    // CRASH_VULKAN_NO_EXT=depth_clip,depth_bias_control,maintenance5,gpl (debug): the
                    // paths of a GPU without them (another vendor's driver) on this one.
                    if (const char* off = getenv("CRASH_VULKAN_NO_EXT"))
                    {
                        m_depthClipExtension = m_depthClipExtension && !strstr(off, "depth_clip");
                        m_depthBiasControl = m_depthBiasControl && !strstr(off, "depth_bias_control");
                        m_maintenance5 = m_maintenance5 && !strstr(off, "maintenance5");
                        m_gplExtension = m_gplExtension && !strstr(off, "gpl");
                    }
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
                      (d24.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !getenv("CRASH_VULKAN_D32") && !(m_safe & SAFE_DEPTH);
            m_uploadSize = std::min<uint64_t>(UPLOAD_RING_SIZE, m_properties.limits.maxStorageBufferRange & ~uint64_t(0xFFFF));
            // The ring is also read as dynamic uniform buffers (the draws' constants, at offsets into
            // the whole ring). The spec limits only each read to maxUniformBufferRange, not the offset,
            // but the Intel UHD driver (128 MB) reads zeros past it: with 256 MB, every draw of the
            // frames whose constants were in the upper half came out empty (black for ~12 frames out
            // of ~26, issue #3). A limit that small can't be a window (NVIDIA: 64 KB) is left alone.
            if (uint64_t window = m_properties.limits.maxUniformBufferRange & ~uint64_t(0xFFFF); window >= (32ull << 20))
                m_uploadSize = std::min(m_uploadSize, window);
            // CRASH_VULKAN_RING_MB=<n> (debug): a smaller upload ring, to test the ring under pressure
            // (a slow GPU keeps more frames' uploads alive).
            if (const char* ring = getenv("CRASH_VULKAN_RING_MB"))
                m_uploadSize = std::clamp<uint64_t>(strtoull(ring, nullptr, 10), 4, 256) << 20;
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
            // Graphics pipeline libraries (asynchronous shader compilation): only with fast linking,
            // the point of them here (a draw's pipeline linked from compiled parts in well under a ms).
            // Enabled whenever the driver has them, used in the asynchronous modes (the setting
            // applies at once).
            VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
            if (m_gplExtension)
            {
                VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
                VkPhysicalDeviceFeatures2 queryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &query };
                vkGetPhysicalDeviceFeatures2(m_physical, &queryFeatures);
                VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
                VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &gplProperties };
                vkGetPhysicalDeviceProperties2(m_physical, &properties);
                m_gplAvailable = query.graphicsPipelineLibrary && gplProperties.graphicsPipelineLibraryFastLinking;
                if (m_gplAvailable)
                {
                    extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
                    extensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
                    gpl.graphicsPipelineLibrary = VK_TRUE;
                    *chain = &gpl;
                    chain = &gpl.pNext;
                }
                else
                {
                    Log("[vulkan] graphics pipeline libraries %s: compiled whole", query.graphicsPipelineLibrary ? "without fast linking" : "unavailable");
                }
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

            if (m_memoryBudget)
                extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // the heaps' use in LogStats

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
            m_enabledDeviceExtensions.assign(extensions.begin(), extensions.end());
            // CRASH_GPU_DEBUG=selftest: calls the layers must report, so a run shows that their
            // messages reach the log (a clean run proves nothing otherwise).
            if (m_debug && getenv("CRASH_GPU_DEBUG") && strstr(getenv("CRASH_GPU_DEBUG"), "selftest"))
            {
                VkBufferCreateInfo empty{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };  // size 0: invalid
                empty.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VkBuffer buffer = VK_NULL_HANDLE;
                if (vkCreateBuffer(m_device, &empty, nullptr, &buffer) == VK_SUCCESS && buffer)
                    vkDestroyBuffer(m_device, buffer, nullptr);
                Log("[vulkan] validation self-test: %llu message(s) so far", (unsigned long long)g_validationMessages);
            }
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
                m_presentMode = supports(VK_PRESENT_MODE_IMMEDIATE_KHR) && !(m_safe & SAFE_PRESENT) ? VK_PRESENT_MODE_IMMEDIATE_KHR
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
            // Transfers: screenshots read the back buffer back. Only the uses the surface has.
            create.imageUsage = (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &
                                capabilities.supportedUsageFlags;
            if (!(create.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            {
                Log("[vulkan] the window surface can't be rendered to (usage 0x%X)", capabilities.supportedUsageFlags);
                return false;
            }
            create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create.preTransform = capabilities.currentTransform;
#if defined(__ANDROID__)
            // A phone in landscape reports ROTATE_90/270 (its panel is portrait). The frames are drawn
            // unrotated, so the compositor turns them (identity); drawing them pre-rotated would save
            // it that pass (docs/internals/android_port.md).
            if (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            if (capabilities.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                Log("[vulkan] surface transform 0x%X (the compositor rotates)", unsigned(capabilities.currentTransform));
#endif
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
            m_stats.swapchains++;
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

        // Driver version as the vendor writes it (NVIDIA 581.57, Intel on Windows 101.6130).
        std::string DriverVersionText(const VkPhysicalDeviceProperties& properties)
        {
            uint32_t v = properties.driverVersion;
            char text[48];
            if (properties.vendorID == 0x10DE)
                snprintf(text, sizeof(text), "%u.%02u", v >> 22, (v >> 14) & 0xFF);
            else if (properties.vendorID == 0x8086)
                snprintf(text, sizeof(text), "%u.%u", v >> 14, v & 0x3FFF);
            else
                snprintf(text, sizeof(text), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
            return text;
        }

        // The formats the renderer creates images in.
        constexpr DXGI_FORMAT IMAGE_FORMATS[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_FLOAT,
            DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM,
            DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
            DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        };

        // What the bug reports need: the driver, the memory, the limits that differ between vendors.
        // Always a few lines; [video] gpu_diagnostics adds the extensions, formats and the surface.
        void VulkanDevice::LogDiagnostics()
        {
            VkPhysicalDevicePushDescriptorPropertiesKHR push{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
            VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &push };
            VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
            vkGetPhysicalDeviceProperties2(m_physical, &properties);
            const VkPhysicalDeviceLimits& limits = m_properties.limits;
            const char* type = m_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
                               : m_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                                                   : "other";
            Log("[vulkan] driver %s (%s) %s, id %u, conformance %u.%u.%u.%u; vendor 0x%04X device 0x%04X, %s GPU", driver.driverName, driver.driverInfo,
                DriverVersionText(m_properties).c_str(), uint32_t(driver.driverID), driver.conformanceVersion.major, driver.conformanceVersion.minor,
                driver.conformanceVersion.subminor, driver.conformanceVersion.patch, m_properties.vendorID, m_properties.deviceID, type);
            std::string heaps;
            for (uint32_t i = 0; i < m_memoryProperties.memoryHeapCount; i++)
            {
                char text[96];
                snprintf(text, sizeof(text), "%s%u: %llu MB%s", i ? ", " : "", i, (unsigned long long)(m_memoryProperties.memoryHeaps[i].size >> 20),
                         (m_memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
                heaps += text;
            }
            Log("[vulkan] memory heaps: %s", heaps.c_str());
            Log("[vulkan] limits: 2D images %u, push descriptors %u, sampled images per stage %u, storage buffer range %u MB, uniform buffer range %u, "
                "uniform offset alignment %llu, buffer/image granularity %llu, timestamp period %.2f ns",
                limits.maxImageDimension2D, push.maxPushDescriptors, limits.maxPerStageDescriptorSampledImages, limits.maxStorageBufferRange >> 20,
                limits.maxUniformBufferRange, (unsigned long long)limits.minUniformBufferOffsetAlignment, (unsigned long long)limits.bufferImageGranularity,
                limits.timestampPeriod);
            if (push.maxPushDescriptors < 32)
                Log("[vulkan] warning: %u push descriptors, the draws bind 32 textures", push.maxPushDescriptors);
            if (!m_diagnostics)
                return;

            std::string extensions;
            for (const std::string& name : m_enabledDeviceExtensions)
                extensions += (extensions.empty() ? "" : " ") + name;
            Log("[vulkan] diagnostics: device extensions: %s", extensions.c_str());
            Log("[vulkan] diagnostics: anisotropy %d (max %.0f), depth clamp %d, mirror clamp to edge %d, depth clip %d, depth bias control %d, "
                "maintenance5 %d, memory budget %d, D24S8 sampled %d, upload ring %llu MB",
                int(m_anisotropy), m_maxAnisotropy, int(m_depthClamp), int(m_mirrorClampToEdge), int(m_depthClipExtension), int(m_depthBiasControl),
                int(m_maintenance5), int(m_memoryBudget), int(m_d24s8), (unsigned long long)(m_uploadSize >> 20));
            for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; i++)
                Log("[vulkan] diagnostics: memory type %u: heap %u, flags 0x%X", i, m_memoryProperties.memoryTypes[i].heapIndex,
                    m_memoryProperties.memoryTypes[i].propertyFlags);
            // Optimal-tiling features per format: S sampled, L linear filter, C color attachment, B
            // blend, D depth/stencil, U storage, T transfer source and destination.
            std::string formats;
            for (DXGI_FORMAT format : IMAGE_FORMATS)
            {
                VkFormatProperties props;
                vkGetPhysicalDeviceFormatProperties(m_physical, VkFormatOf(format), &props);
                VkFormatFeatureFlags f = props.optimalTilingFeatures;
                char text[48];
                snprintf(text, sizeof(text), "%s%u:%s%s%s%s%s%s%s", formats.empty() ? "" : " ", uint32_t(format),
                         (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "S" : "", (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? "L" : "",
                         (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "C" : "", (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? "B" : "",
                         (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "D" : "", (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "U" : "",
                         (f & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && (f & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? "T" : "");
                formats += text;
            }
            Log("[vulkan] diagnostics: formats (DXGI: S sampled, L linear, C color, B blend, D depth, U storage, T transfer): %s", formats.c_str());
            VkSurfaceCapabilitiesKHR capabilities{};
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &capabilities);
            uint32_t modeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &modeCount, nullptr);
            std::vector<VkPresentModeKHR> modes(modeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &modeCount, modes.data());
            std::string modeList;
            for (VkPresentModeKHR mode : modes)
                modeList += (modeList.empty() ? "" : " ") + std::to_string(int(mode));
            Log("[vulkan] diagnostics: surface: images %u-%u, extent %ux%u, usage 0x%X, transforms 0x%X, composite alpha 0x%X, present modes %s "
                "(0 immediate, 1 mailbox, 2 fifo, 3 fifo relaxed)",
                capabilities.minImageCount, capabilities.maxImageCount, capabilities.currentExtent.width, capabilities.currentExtent.height,
                capabilities.supportedUsageFlags, capabilities.supportedTransforms, capabilities.supportedCompositeAlpha, modeList.c_str());
        }

        bool VulkanDevice::Init(const DeviceInfo& info)
        {
            m_safe = info.safeMode;
            m_diagnostics = info.diagnostics;
            m_compileMode = info.shaderCompilation;
            // CRASH_SHADER_COMPILE=sync|async|aggressive overrides [video] shader_compilation.
            if (const char* mode = getenv("CRASH_SHADER_COMPILE"))
                m_compileMode = ParseShaderCompilation(mode);
            if (m_safe)
                Log("[vulkan] GPU safe mode: %s", SafeModeNames(m_safe).c_str());
            m_shaderCache = info.shaderCache;
            m_extraInstanceExtensions = info.instanceExtensions;
            m_extraDeviceExtensions = info.deviceExtensions;
#if defined(__ANDROID__)
            // No DXC for Android yet: the SPIR-V made on the PC (the shader cache pushed to
            // userdata/shadercache) is all there is; a shader missing from it isn't drawn
            // (android_port.md, "Shaders").
            if (!vulkan::CompilerAvailable())
                Log("[vulkan] no shader compiler: cached SPIR-V only");
#else
            if (!vulkan::CompilerAvailable())
                return false;
#endif
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
            m_gpl = m_gplAvailable && m_compileMode != ShaderCompilation::Sync;
            Log("[vulkan] shader compilation: %s; graphics pipeline libraries %s", m_compileMode == ShaderCompilation::Sync ? "sync" : m_compileMode == ShaderCompilation::Aggressive ? "aggressive" : "async",
                m_gplAvailable ? "with fast linking (a new pipeline is linked from compiled parts, optimized in the background)" : "unavailable (whole pipelines)");
            LogDiagnostics();

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
            if (!m_shaderCache.empty() && !(m_safe & SAFE_CACHE))
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
            if (m_compileMode != ShaderCompilation::Sync)
            {
                StartCompileWorkers();
                StartPrecompile();
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
            if (const char* profile = getenv("CRASH_GPU_PROFILE"); profile && m_timestampFrequency > 0.0)
            {
                char* end;
                m_profileFirst = strtoull(profile, &end, 10);
                m_profileLast = *end == '-' ? strtoull(end + 1, nullptr, 10) : m_profileFirst;
                queries.queryCount = PROFILE_QUERIES;
                if (vkCreateQueryPool(m_device, &queries, nullptr, &m_profilePool) != VK_SUCCESS)
                    m_profileLast = 0;
                m_profiling = m_profilePool && m_profileFirst <= 1 && m_profileLast >= 1;
                m_profileDraws = getenv("CRASH_GPU_PROFILE_DRAWS") != nullptr;
            }

            if (!CreateSwapchain(info.width, info.height))
                return false;
            if (m_debug && getenv("CRASH_GPU_DEBUG") && strstr(getenv("CRASH_GPU_DEBUG"), "selftest"))
            {
                // Two clears of an image with no barrier between them: synchronization validation
                // must report a write-after-write hazard.
                Transition(m_nullTexture, Access::TransferDst);
                FlushBarriers();
                VkClearColorValue value{};
                VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(m_cmd, m_nullTexture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
                vkCmdClearColorImage(m_cmd, m_nullTexture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
                Transition(m_nullTexture, Access::ShaderRead);
                Log("[vulkan] validation self-test (synchronization): %llu message(s) so far", (unsigned long long)g_validationMessages);
            }
            if (const char* load = getenv("CRASH_VULKAN_GPU_LOAD"); load && atoi(load) > 0)
            {
                PassPipelineDesc desc;
                desc.name = "gpu_load";
                desc.ps = R"(
cbuffer Load : register(b0) { uint iterations; uint3 unused; };
float4 main(float4 position : SV_Position) : SV_Target
{
    float v = position.x * 0.001;
    for (uint i = 0; i < iterations; i++)
        v = frac(v * 1.0001 + 0.37);
    return float4(v, v, v, 1);
}
)";
                desc.targets[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.constantBytes = 16;
                TextureDesc target;
                target.width = target.height = 64;
                target.format = DXGI_FORMAT_R8G8B8A8_UNORM;
                target.usage = USAGE_RENDER_TARGET;
                target.name = "gpu load";
                m_gpuLoadPipeline = static_cast<VulkanPipeline*>(CreatePassPipeline(desc));
                m_gpuLoadTarget = static_cast<VulkanTexture*>(CreateTexture(target));
                if (m_gpuLoadPipeline && m_gpuLoadTarget)
                {
                    m_gpuLoad = uint32_t(atoi(load)) * 1000u;
                    Log("[vulkan] debug: %u iterations of GPU load per frame (CRASH_VULKAN_GPU_LOAD)", m_gpuLoad);
                }
            }
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
            else if (result == VK_ERROR_DEVICE_LOST)  // every later wait fails too (the upload ring's loop would spin)
                Fatal("[vulkan] device lost (the GPU stopped or the driver reset it) waiting for submission %llu", (unsigned long long)value);
            else
                Check(result, "vkWaitSemaphores");
            CompletedValue();
        }

        void VulkanDevice::Collect()
        {
            uint64_t completed = CompletedValue();
            m_uploadRing.Retire(completed);
            if (!m_probes.empty())
                CollectProbes(completed);
            if (m_profilePool)
                CollectProfile(completed);
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
            ProfileEnd();
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
            VkSemaphoreSubmitInfo waits[2]{};
            uint32_t waitCount = 0;
            VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &command;
            submit.signalSemaphoreInfoCount = signalCount;
            submit.pSignalSemaphoreInfos = signals;
            if (m_pendingAcquire)
            {
                waits[waitCount++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, m_pendingAcquire, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
                m_acquireValues[(m_acquireIndex + ACQUIRE_SEMAPHORES - 1) % ACQUIRE_SEMAPHORES] = m_timelineValue;
                m_pendingAcquire = VK_NULL_HANDLE;
            }
            submit.waitSemaphoreInfoCount = waitCount;
            submit.pWaitSemaphoreInfos = waitCount ? waits : nullptr;
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
            bool submitted = false;
            while (offset == UINT64_MAX)
            {
                Collect();
                offset = m_uploadRing.TryAllocate(size, alignment);
                if (offset != UINT64_MAX)
                    break;
                if (size > m_uploadSize)
                    return {};
                int64_t waitStart, waitEnd;
                waitStart = int64_t(plat::PerfCounter());
                m_stats.ringWaits++;
                if (m_uploadRing.HasPending())
                    WaitValue(m_uploadRing.OldestPendingFence());
                else if (!submitted)
                {
                    // The recorded commands hold the rest of the ring: submit them and wait.
                    submitted = true;
                    m_stats.ringSubmits++;
                    WaitIdle();
                }
                else
                {
                    // Uploads of commands not recorded yet fill it (not expected).
                    static uint32_t logged;
                    if (logged++ < 4)
                        Log("[vulkan] upload ring full (%llu MB): %llu bytes not available", (unsigned long long)(m_uploadSize >> 20),
                            (unsigned long long)size);
                    return {};
                }
                waitEnd = int64_t(plat::PerfCounter());
                m_timing.ringWaitTicks += uint64_t(waitEnd - waitStart);
            }
            m_stats.ringPeak = std::max(m_stats.ringPeak, m_uploadRing.Used());
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
            if (requirements.size <= BLOCK_SIZE / 4 && !dedicatedOnly && !(m_safe & SAFE_MEMORY))
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
            {
                // A use the driver doesn't support for the format (once per format and use): such an
                // image may be created anyway and then sample or render garbage or black.
                auto [it, inserted] = m_formatFeatures.try_emplace(uint32_t(texture->renderFormat) | (uint32_t(image.usage) << 24), 0);
                if (inserted)
                {
                    VkFormatProperties props, renderProps;
                    vkGetPhysicalDeviceFormatProperties(m_physical, texture->format, &props);
                    vkGetPhysicalDeviceFormatProperties(m_physical, texture->renderFormat, &renderProps);
                    VkFormatFeatureFlags have = props.optimalTilingFeatures, haveRender = renderProps.optimalTilingFeatures;
                    std::string missing;
                    if (!(have & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                        missing += " sampled";
                    if (!(have & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) || !(have & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
                        missing += " transfer";
                    if ((image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(haveRender & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                        missing += " color-attachment";
                    if ((image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(haveRender & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
                        missing += " blend";
                    if ((image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(have & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
                        missing += " depth-attachment";
                    if ((image.usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(have & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
                        missing += " storage";
                    if (!missing.empty())
                        Log("[vulkan] warning: format %u (DXGI %u, %s) lacks:%s", uint32_t(texture->renderFormat), uint32_t(desc.format), desc.name,
                            missing.c_str());
                }
            }
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
            // A swap chain image just acquired: the transition waits for the acquire semaphore (its
            // wait stage, all commands, includes this one), so it doesn't race the presentation engine.
            if (texture->swapchain && (texture->access == Access::Undefined || texture->access == Access::Present))
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            if (m_safe & SAFE_BARRIERS)
            {
                // Everything before, every kind of memory access: rules out a wrong stage or access mask.
                barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            }
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
            if (!m_rendering.active || !m_barriers.empty() || pipeline->pass || colorCount > m_rendering.colorCount || (m_safe & SAFE_INSTANCES))
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
            auto* variant = static_cast<VulkanPipeline*>(GetDrawPipeline(key, false, nullptr));
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
            if (m_profiling)
                ProfileStamp(m_profileOperation ? m_profileOperation : "draws", colorCount && colors[0] ? colors[0] : depth);
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
            EndRendering();
            if (m_profiling)
                ProfileStamp("upload", vulkanTexture);
            FlushBarriers();
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
            EndRendering();
            if (m_profiling)
                ProfileStamp("copy", to);
            FlushBarriers();
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
                m_profileOperation = "clear";
                BeginRendering(colors, 1, nullptr);
                m_profileOperation = nullptr;
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
            {
                m_profileOperation = "clear";
                BeginRendering(nullptr, 0, vulkanTexture);
                m_profileOperation = nullptr;
            }
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
            // The same source again, or a shader the compile workers loaded for a recorded pipeline.
            uint64_t hash = vulkan::SpirvHash(stage, hlsl, layout);
            if (VulkanShader* known = ShaderByHash(hash))
                return known;
            std::vector<uint32_t> spirv;
            uint64_t compiles = m_stats.shaderCompiles;
            int64_t start, end;
            start = int64_t(plat::PerfCounter());
            bool compiled = vulkan::CompileSpirv(stage, hlsl, name, layout, m_shaderCache, spirv, &m_stats.shaderCompiles, &m_stats.shaderFailures);
            end = int64_t(plat::PerfCounter());
            if (m_stats.shaderCompiles != compiles)
            {
                m_timing.shaders++;
                m_timing.shaderTicks += uint64_t(end - start);
            }
            if (!compiled)
                return nullptr;
            auto shader = std::make_unique<VulkanShader>();
            shader->stage = stage;
            shader->hash = hash;
            VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            create.codeSize = spirv.size() * 4;
            create.pCode = spirv.data();
            if (!Check(vkCreateShaderModule(m_device, &create, nullptr, &shader->module), "vkCreateShaderModule"))
                return nullptr;
            std::lock_guard<std::mutex> lock(m_programMutex);
            auto [it, added] = m_shaderByHash.emplace(hash, shader.get());
            if (!added)
            {
                vkDestroyShaderModule(m_device, shader->module, nullptr);  // a worker loaded it meanwhile
                return it->second;
            }
            m_shaders.push_back(std::move(shader));
            return m_shaders.back().get();
        }

        VulkanShader* VulkanDevice::ShaderByHash(uint64_t hash)
        {
            std::lock_guard<std::mutex> lock(m_programMutex);
            auto it = m_shaderByHash.find(hash);
            return it != m_shaderByHash.end() ? it->second : nullptr;
        }

        // A recorded pipeline's shader (compile workers): its SPIR-V from the cache; null if it isn't
        // there (a translator or graphics option change made other sources since).
        VulkanShader* VulkanDevice::LoadCachedSpirv(uint64_t hash, Stage stage)
        {
            if (VulkanShader* known = ShaderByHash(hash))
                return known;
            std::vector<uint8_t> spirv;
            if (m_shaderCache.empty() || !ReadCachedShader(m_shaderCache, hash, "spv", spirv) || spirv.empty() || spirv.size() % 4)
                return nullptr;
            auto shader = std::make_unique<VulkanShader>();
            shader->stage = stage;
            shader->hash = hash;
            VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            create.codeSize = spirv.size();
            create.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
            if (!Check(vkCreateShaderModule(m_device, &create, nullptr, &shader->module), "vkCreateShaderModule"))
                return nullptr;
            std::lock_guard<std::mutex> lock(m_programMutex);
            auto [it, added] = m_shaderByHash.emplace(hash, shader.get());
            if (!added)
            {
                vkDestroyShaderModule(m_device, shader->module, nullptr);
                return it->second;
            }
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

        constexpr uint32_t PART_VERTEX_INPUT = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        constexpr uint32_t PART_PRE_RASTERIZATION = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
        constexpr uint32_t PART_FRAGMENT_SHADER = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
        constexpr uint32_t PART_FRAGMENT_OUTPUT = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        constexpr uint32_t ALL_PARTS = PART_VERTEX_INPUT | PART_PRE_RASTERIZATION | PART_FRAGMENT_SHADER | PART_FRAGMENT_OUTPUT;

        // The fields of a ProgramKey that one part (a pipeline library) is made from.
        ProgramKey LibraryKey(const ProgramKey& key, uint32_t part)
        {
            ProgramKey library;
            switch (part)
            {
            case PART_VERTEX_INPUT:
                library.topologyClass = key.topologyClass;
                library.library = 1;
                break;
            case PART_PRE_RASTERIZATION:
                library.vs = key.vs;
                library.gs = key.gs;
                library.depthClip = key.depthClip;
                library.library = 2;
                break;
            case PART_FRAGMENT_SHADER:
                library.ps = key.ps;
                library.library = 3;
                break;
            default:
                memcpy(library.colorFormats, key.colorFormats, sizeof(key.colorFormats));
                library.depthFormat = key.depthFormat;
                library.colorCount = key.colorCount;
                memcpy(library.blend, key.blend, sizeof(key.blend));
                library.colorMask = key.colorMask;
                library.library = 4;
                break;
            }
            return library;
        }

        ProgramKey VulkanDevice::MakeProgramKey(const DrawPipelineKey& key) const
        {
            ProgramKey program;
            program.vs = static_cast<VulkanShader*>(key.vs)->hash;
            program.gs = key.gs ? static_cast<VulkanShader*>(key.gs)->hash : 0;
            program.ps = key.ps ? static_cast<VulkanShader*>(key.ps)->hash : 0;
            program.colorCount = std::min(key.colorCount, 4u);
            for (uint32_t i = 0; i < program.colorCount; i++)
            {
                uint32_t mask = (key.colorMask >> (4 * i)) & 0xF;
                // A slot the draw doesn't write has no attachment (Draw binds a null view there), unless
                // the draw runs inside a rendering instance with more attachments (keepFormats).
                bool attached = key.keepFormats ? key.colorFormats[i] != 0 : mask != 0;
                program.colorFormats[i] = attached ? key.colorFormats[i] : 0;
                program.blend[i] = mask ? key.blend[i] : 0x00010001;  // unwritten: opaque, so pipelines are shared
                program.colorMask |= mask << (4 * i);
            }
            if (key.depthFormat)
            {
                DXGI_FORMAT depthFormat = DXGI_FORMAT(key.depthFormat);
                if (depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT && !m_d24s8)
                    depthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                program.depthFormat = uint32_t(depthFormat);
            }
            program.topologyClass = key.topology == Topology::PointList ? 0 : (key.topology == Topology::LineList || key.topology == Topology::LineStrip) ? 1 : 2;
            program.depthClip = key.depthClip;
            return program;
        }

        DrawDynamic VulkanDevice::MakeDrawDynamic(const DrawPipelineKey& key) const
        {
            DrawDynamic dynamic;
            dynamic.cull = uint8_t(key.cull == Cull::Front ? VK_CULL_MODE_FRONT_BIT : key.cull == Cull::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
            dynamic.topology = uint8_t(PrimitiveTopology(key.topology));
            dynamic.restart = key.stripCut && (key.topology == Topology::TriangleStrip || key.topology == Topology::LineStrip);
            if (key.depthFormat)
            {
                uint32_t depthControl = key.depthControl;
                dynamic.depthTest = (depthControl >> 1) & 1;
                dynamic.depthWrite = (depthControl >> 2) & 1;
                dynamic.depthCompare = (depthControl >> 4) & 7;
                dynamic.stencilTest = depthControl & 1;
                // compare | fail << 8 | pass << 16 | depth fail << 24 (VkCompareOp and VkStencilOp have the Xenos numbering).
                auto ops = [](uint32_t compare, uint32_t fail, uint32_t pass, uint32_t depthFail) { return compare | fail << 8 | pass << 16 | depthFail << 24; };
                dynamic.frontOps = ops((depthControl >> 8) & 7, (depthControl >> 11) & 7, (depthControl >> 14) & 7, (depthControl >> 17) & 7);
                dynamic.backOps = (depthControl >> 7) & 1 ? ops((depthControl >> 20) & 7, (depthControl >> 23) & 7, (depthControl >> 26) & 7, (depthControl >> 29) & 7)
                                                          : dynamic.frontOps;
                dynamic.compareMask = (key.stencilMasks >> 8) & 0xFF;
                dynamic.writeMask = (key.stencilMasks >> 16) & 0xFF;
                dynamic.biasEnable = key.depthBias != 0 || key.slopeBias != 0.0f;
                dynamic.biasConstant = float(key.depthBias);
                dynamic.biasSlope = key.slopeBias;
                dynamic.biasUnorm = dynamic.biasEnable && key.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT && !m_d24s8 && m_depthBiasControl;
            }
            return dynamic;
        }

        void VulkanDevice::SetDrawDynamic(const DrawDynamic& dynamic)
        {
            DrawState& state = m_drawState;
            const DrawDynamic& last = state.drawDynamic;
            bool all = !state.drawDynamicValid;
            if (!all && memcmp(&last, &dynamic, sizeof(dynamic)) == 0)
                return;
            if (all || last.cull != dynamic.cull)
                vkCmdSetCullMode(m_cmd, dynamic.cull);
            if (all || last.topology != dynamic.topology)
                vkCmdSetPrimitiveTopology(m_cmd, VkPrimitiveTopology(dynamic.topology));
            if (all || last.restart != dynamic.restart)
                vkCmdSetPrimitiveRestartEnable(m_cmd, dynamic.restart);
            if (all || last.depthTest != dynamic.depthTest)
                vkCmdSetDepthTestEnable(m_cmd, dynamic.depthTest);
            if (all || last.depthWrite != dynamic.depthWrite)
                vkCmdSetDepthWriteEnable(m_cmd, dynamic.depthWrite);
            if (all || last.depthCompare != dynamic.depthCompare)
                vkCmdSetDepthCompareOp(m_cmd, VkCompareOp(dynamic.depthCompare));
            if (all || last.stencilTest != dynamic.stencilTest)
                vkCmdSetStencilTestEnable(m_cmd, dynamic.stencilTest);
            auto stencilOps = [&](VkStencilFaceFlags face, uint32_t ops) {
                vkCmdSetStencilOp(m_cmd, face, VkStencilOp((ops >> 8) & 0xFF), VkStencilOp((ops >> 16) & 0xFF), VkStencilOp(ops >> 24), VkCompareOp(ops & 0xFF));
            };
            if (all || last.frontOps != dynamic.frontOps)
                stencilOps(VK_STENCIL_FACE_FRONT_BIT, dynamic.frontOps);
            if (all || last.backOps != dynamic.backOps)
                stencilOps(VK_STENCIL_FACE_BACK_BIT, dynamic.backOps);
            if (all || last.compareMask != dynamic.compareMask)
                vkCmdSetStencilCompareMask(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dynamic.compareMask);
            if (all || last.writeMask != dynamic.writeMask)
                vkCmdSetStencilWriteMask(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dynamic.writeMask);
            if (all || last.biasEnable != dynamic.biasEnable)
                vkCmdSetDepthBiasEnable(m_cmd, dynamic.biasEnable);
            if (all || last.biasConstant != dynamic.biasConstant || last.biasSlope != dynamic.biasSlope || last.biasUnorm != dynamic.biasUnorm)
            {
                if (dynamic.biasUnorm)
                {
                    // The 24-bit depth's bias units on the D32 depth buffer that stands in for it.
                    VkDepthBiasRepresentationInfoEXT representation{ VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT };
                    representation.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
                    VkDepthBiasInfoEXT bias{ VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT, &representation };
                    bias.depthBiasConstantFactor = dynamic.biasConstant;
                    bias.depthBiasSlopeFactor = dynamic.biasSlope;
                    vkCmdSetDepthBias2EXT(m_cmd, &bias);
                }
                else
                {
                    vkCmdSetDepthBias(m_cmd, dynamic.biasConstant, 0.0f, dynamic.biasSlope);
                }
            }
            state.drawDynamic = dynamic;
            state.drawDynamicValid = true;
        }

        Program* VulkanDevice::FindProgram(const ProgramKey& key, bool record)
        {
            uint64_t hash = HashBytes(&key, sizeof(key));
            std::lock_guard<std::mutex> lock(m_programMutex);
            std::unique_ptr<Program>& slot = m_programs[hash];
            if (!slot)
            {
                slot = std::make_unique<Program>();
                slot->key = key;
                if (record && !m_shaderCache.empty() && m_recorded.insert(hash).second)
                    m_newRecords.push_back(key);
            }
            return slot.get();
        }

        Lazy* VulkanDevice::FindLibrary(const ProgramKey& key, uint32_t part)
        {
            ProgramKey library = LibraryKey(key, part);
            uint64_t hash = HashBytes(&library, sizeof(library));
            std::lock_guard<std::mutex> lock(m_programMutex);
            std::unique_ptr<Lazy>& slot = m_libraries[hash];
            if (!slot)
                slot = std::make_unique<Lazy>();
            return slot.get();
        }

        bool VulkanDevice::Libraries(const ProgramKey& key, Lazy* libraries[4])
        {
            const uint32_t parts[4] = { PART_VERTEX_INPUT, PART_PRE_RASTERIZATION, PART_FRAGMENT_SHADER, PART_FRAGMENT_OUTPUT };
            bool built = true;
            for (uint32_t i = 0; i < 4; i++)
            {
                libraries[i] = FindLibrary(key, parts[i]);
                built = built && libraries[i]->Built();
            }
            return built;
        }

        VkPipeline VulkanDevice::Build(Lazy& lazy, const std::function<VkPipeline()>& build)
        {
            uint8_t state = lazy.state.load(std::memory_order_acquire);
            for (;;)
            {
                if (state == Lazy::BUILT)
                    return lazy.handle;
                if (state == Lazy::BUILDING)
                {
                    std::unique_lock<std::mutex> lock(m_lazyMutex);
                    m_lazyBuilt.wait(lock, [&] { return lazy.Built(); });
                    return lazy.handle;
                }
                // Idle, or queued for a worker that hasn't started it: built here.
                if (lazy.state.compare_exchange_weak(state, Lazy::BUILDING, std::memory_order_acq_rel))
                {
                    VkPipeline handle = build();
                    {
                        std::lock_guard<std::mutex> lock(m_lazyMutex);
                        lazy.handle = handle;
                        lazy.state.store(Lazy::BUILT, std::memory_order_release);
                    }
                    m_lazyBuilt.notify_all();
                    return handle;
                }
            }
        }

        void VulkanDevice::Queue(Lazy& lazy, bool urgent, std::function<VkPipeline()> build)
        {
            uint8_t idle = Lazy::IDLE;
            if (!lazy.state.compare_exchange_strong(idle, Lazy::QUEUED, std::memory_order_acq_rel))
                return;
            // Built already if another thread needed it first (Build then just returns it).
            Enqueue(urgent, [this, &lazy, build = std::move(build)] { Build(lazy, build); });
        }

        VkPipeline VulkanDevice::CreateGraphics(const ProgramKey& key, uint32_t parts, const VkPipeline* libraries, bool optimize)
        {
            VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            create.layout = m_drawLayout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (libraries)
            {
                // Linked from the four parts: fast, or optimized across them (on a compile worker).
                VkPipelineLibraryCreateInfoKHR link{ VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
                link.libraryCount = 4;
                link.pLibraries = libraries;
                create.pNext = &link;
                create.flags = optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
                VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &create, nullptr, &pipeline);
                if (result != VK_SUCCESS)
                {
                    Log("[vulkan] linking a pipeline failed (%s) for vs %016llx ps %016llx", ResultName(result), (unsigned long long)key.vs, (unsigned long long)key.ps);
                    m_compileStats.failures++;
                    return VK_NULL_HANDLE;
                }
                (optimize ? m_compileStats.optimized : m_compileStats.fastLinks)++;
                m_unsavedPipelines++;
                return pipeline;
            }
            bool vertexInputPart = parts & PART_VERTEX_INPUT, preRasterization = parts & PART_PRE_RASTERIZATION;
            bool fragmentShader = parts & PART_FRAGMENT_SHADER, fragmentOutput = parts & PART_FRAGMENT_OUTPUT;

            VkPipelineShaderStageCreateInfo stages[3]{};
            uint32_t stageCount = 0;
            auto addStage = [&](uint64_t hash, VkShaderStageFlagBits stage) {
                VulkanShader* shader = ShaderByHash(hash);
                if (!shader)
                    return false;
                stages[stageCount] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                stages[stageCount].stage = stage;
                stages[stageCount].module = shader->module;
                stages[stageCount].pName = "main";
                stageCount++;
                return true;
            };
            if (preRasterization && (!addStage(key.vs, VK_SHADER_STAGE_VERTEX_BIT) || (key.gs && !addStage(key.gs, VK_SHADER_STAGE_GEOMETRY_BIT))))
                return VK_NULL_HANDLE;
            if (fragmentShader && key.ps && !addStage(key.ps, VK_SHADER_STAGE_FRAGMENT_BIT))
                return VK_NULL_HANDLE;
            // CRASH_VULKAN_COLD=1 (measurements): an unused specialization constant with a value of
            // this run, so the driver's own shader cache misses as on a first run.
            static const uint32_t s_nonce = getenv("CRASH_VULKAN_COLD") ? uint32_t(plat::TickCountMs() * 2654435761ull) | 1u : 0u;
            static const VkSpecializationMapEntry s_nonceEntry{ 4000, 0, 4 };
            static const VkSpecializationInfo s_nonceInfo{ 1, &s_nonceEntry, 4, &s_nonce };
            for (uint32_t i = 0; i < stageCount && s_nonce; i++)
                stages[i].pSpecializationInfo = &s_nonceInfo;

            // Vertex input: none (the shaders fetch from the upload ring); the topology's class (the
            // topology itself is dynamic).
            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            inputAssembly.topology = key.topologyClass == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : key.topologyClass == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.frontFace = VK_FRONT_FACE_CLOCKWISE;  // as D3D12's FrontCounterClockwise = FALSE (viewport flipped)
            raster.lineWidth = 1.0f;
            VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClip{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
            if (!key.depthClip)
            {
                if (m_depthClipExtension)
                {
                    depthClip.depthClipEnable = VK_FALSE;
                    raster.pNext = &depthClip;
                }
                else if (m_depthClamp)
                {
                    raster.depthClampEnable = VK_TRUE;
                }
            }

            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };  // dynamic

            VkPipelineColorBlendAttachmentState blends[4]{};
            VkFormat colorFormats[4]{};
            for (uint32_t i = 0; i < key.colorCount; i++)
            {
                colorFormats[i] = key.colorFormats[i] ? VkFormatOf(DXGI_FORMAT(key.colorFormats[i])) : VK_FORMAT_UNDEFINED;
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
                blend.colorWriteMask = (key.colorMask >> (4 * i)) & 0xF;
            }
            VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            colorBlend.attachmentCount = key.colorCount;
            colorBlend.pAttachments = blends;

            static const VkDynamicState DYNAMICS[] = {
                VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS,
                VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
            };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = uint32_t(std::size(DYNAMICS));
            dynamic.pDynamicStates = DYNAMICS;

            // Formats: the fragment output's (the other parts only take the view mask, 0).
            VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            if (fragmentOutput)
            {
                rendering.colorAttachmentCount = key.colorCount;
                rendering.pColorAttachmentFormats = colorFormats;
                if (key.depthFormat)
                    rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = VkFormatOf(DXGI_FORMAT(key.depthFormat));
            }

            VkGraphicsPipelineLibraryCreateInfoEXT library{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &rendering };
            library.flags = parts;
            bool whole = parts == ALL_PARTS;
            create.pNext = whole ? static_cast<const void*>(&rendering) : &library;
            create.flags = whole ? 0 : VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
            create.stageCount = stageCount;
            create.pStages = stageCount ? stages : nullptr;
            create.pVertexInputState = vertexInputPart ? &vertexInput : nullptr;
            create.pInputAssemblyState = vertexInputPart ? &inputAssembly : nullptr;
            create.pViewportState = preRasterization ? &viewport : nullptr;
            create.pRasterizationState = preRasterization ? &raster : nullptr;
            create.pMultisampleState = fragmentShader || fragmentOutput ? &multisample : nullptr;
            create.pDepthStencilState = fragmentShader ? &depthStencil : nullptr;
            create.pColorBlendState = fragmentOutput ? &colorBlend : nullptr;
            create.pDynamicState = &dynamic;
            if (!preRasterization && !fragmentShader)
                create.layout = VK_NULL_HANDLE;  // the interface parts have no shaders
            VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &create, nullptr, &pipeline);
            if (result != VK_SUCCESS)
            {
                Log("[vulkan] vkCreateGraphicsPipelines failed (%s) for vs %016llx ps %016llx (parts 0x%X)", ResultName(result), (unsigned long long)key.vs,
                    (unsigned long long)key.ps, parts);
                m_compileStats.failures++;
                return VK_NULL_HANDLE;
            }
            (whole ? m_compileStats.programs : m_compileStats.libraries)++;
            m_unsavedPipelines++;
            return pipeline;
        }

        // Makes `program` drawable: false if it failed, or (`deferred`) if the draw is skipped while
        // a worker compiles it.
        bool VulkanDevice::PrepareProgram(Program& program, bool deferrable, bool& deferred)
        {
            const ProgramKey& key = program.key;
            bool mayWait = deferrable && m_compileMode == ShaderCompilation::Aggressive;
            if (!m_gpl)
            {
                if (mayWait && !program.full.Built())
                {
                    Queue(program.full, true, [this, &program] { return CreateGraphics(program.key, ALL_PARTS); });
                    deferred = true;
                    return false;
                }
                return Build(program.full, [&] { return CreateGraphics(key, ALL_PARTS); }) != VK_NULL_HANDLE;
            }
            if (program.Current())
                return true;
            const uint32_t parts[4] = { PART_VERTEX_INPUT, PART_PRE_RASTERIZATION, PART_FRAGMENT_SHADER, PART_FRAGMENT_OUTPUT };
            Lazy* libraries[4];
            if (!Libraries(key, libraries) && mayWait)
            {
                for (uint32_t i = 0; i < 4; i++)
                    if (!libraries[i]->Built())
                        Queue(*libraries[i], true, [this, key, part = parts[i]] { return CreateGraphics(key, part); });
                deferred = true;
                return false;
            }
            // The parts (each compiled once, shared by every pipeline that has it), then a fast link.
            std::array<VkPipeline, 4> handles{};
            for (uint32_t i = 0; i < 4; i++)
            {
                handles[i] = Build(*libraries[i], [&] { return CreateGraphics(key, parts[i]); });
                if (!handles[i])
                    return false;
            }
            if (!Build(program.fast, [&] { return CreateGraphics(key, 0, handles.data(), false); }))
                return false;
            // The optimized pipeline replaces the fast-linked one once a worker has made it.
            Queue(program.full, true, [this, &program, handles] { return CreateGraphics(program.key, 0, handles.data(), true); });
            return true;
        }

        Pipeline* VulkanDevice::GetDrawPipeline(const DrawPipelineKey& key, bool deferrable, bool* deferred)
        {
            if (deferred)
                *deferred = false;
            uint64_t hash = HashBytes(&key, sizeof(key));
            auto it = m_drawPipelines.find(hash);
            VulkanPipeline* pipeline;
            if (it != m_drawPipelines.end())
            {
                pipeline = it->second.get();
            }
            else
            {
                auto created = std::make_unique<VulkanPipeline>();
                created->key = key;
                created->program = FindProgram(MakeProgramKey(key), true);
                created->dynamic = MakeDrawDynamic(key);
                pipeline = created.get();
                m_drawPipelines[hash] = std::move(created);
            }
            Program& program = *pipeline->program;
            if (program.Current())
                return pipeline;
            if (pipeline->failed)
                return nullptr;
            int64_t start, end;
            start = int64_t(plat::PerfCounter());
            bool waits = false;
            bool ready = PrepareProgram(program, deferrable, waits);
            end = int64_t(plat::PerfCounter());
            if (waits)
            {
                m_compileStats.deferred++;
                if (deferred)
                    *deferred = true;
                return nullptr;
            }
            // Time the recording thread spent compiling (or waiting for a worker that was).
            m_timing.pipelines++;
            m_timing.pipelineTicks += uint64_t(end - start);
            m_compileStats.stalls++;
            m_stats.pipelines++;
            pipeline->failed = !ready;
            return ready ? pipeline : nullptr;
        }

        // Compile workers: low priority threads (the game's own run above normal), urgent jobs first.
        void VulkanDevice::StartCompileWorkers()
        {
            uint32_t cores = std::max(1u, std::thread::hardware_concurrency());
            m_workerCount = std::clamp(cores / 4, 1u, 4u);
            for (uint32_t i = 0; i < m_workerCount; i++)
            {
                std::thread([this] {
                    plat::SetCurrentThreadName("pipeline compile");
                    plat::SetCurrentThreadPriority(plat::ThreadPriority::BelowNormal);
                    for (;;)
                    {
                        std::function<void()> job;
                        {
                            std::unique_lock<std::mutex> lock(m_jobMutex);
                            m_jobReady.wait(lock, [&] { return !m_jobs[0].empty() || !m_jobs[1].empty(); });
                            std::deque<std::function<void()>>& queue = m_jobs[0].empty() ? m_jobs[1] : m_jobs[0];
                            job = std::move(queue.front());
                            queue.pop_front();
                        }
                        job();
                    }
                }).detach();
            }
        }

        void VulkanDevice::Enqueue(bool urgent, std::function<void()> job)
        {
            {
                std::lock_guard<std::mutex> lock(m_jobMutex);
                m_jobs[urgent ? 0 : 1].push_back(std::move(job));
            }
            m_jobReady.notify_one();
        }

        void VulkanDevice::PrewarmShader(Stage stage, const std::string& hlsl)
        {
            if (!m_gpl || stage == Stage::Geometry)
                return;
            uint64_t hash = vulkan::SpirvHash(stage, hlsl, Layout::Draw);
            m_precompileTotal++;
            Enqueue(false, [this, stage, hash] {
                // The part every draw of it has: the vertex shader alone with depth clip, or the pixel shader.
                ProgramKey key;
                (stage == Stage::Pixel ? key.ps : key.vs) = hash;
                uint32_t part = stage == Stage::Pixel ? PART_FRAGMENT_SHADER : PART_PRE_RASTERIZATION;
                if (LoadCachedSpirv(hash, stage))
                    Build(*FindLibrary(key, part), [&] { return CreateGraphics(key, part); });
                PrecompileDone();
            });
        }

        // pipeline_keys.bin: a header and the ProgramKeys of every pipeline drawn with (appended by
        // SavePipelineCache). At start the workers compile them again (from the driver's cache, ~1 ms
        // each; after a driver update, for real), so a level doesn't compile what an earlier run did.
        constexpr uint32_t RECORDS_MAGIC = 0x4B505243;  // "CRPK"
        constexpr uint32_t RECORDS_VERSION = 1;
        constexpr size_t MAX_PRECOMPILE = 8192;  // the most recent records

        void VulkanDevice::StartPrecompile()
        {
            if (m_shaderCache.empty())
                return;
            std::filesystem::path path = m_shaderCache / "pipeline_keys.bin";
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return;
            uint32_t header[2] = {};
            file.read(reinterpret_cast<char*>(header), sizeof(header));
            if (!file || header[0] != RECORDS_MAGIC || header[1] != RECORDS_VERSION)
            {
                file.close();
                std::error_code error;
                std::filesystem::remove(path, error);
                Log("[vulkan] %s: another format, started again", path.filename().string().c_str());
                return;
            }
            std::vector<ProgramKey> records;
            ProgramKey key;
            while (file.read(reinterpret_cast<char*>(&key), sizeof(key)))
                records.push_back(key);
            if (records.size() > MAX_PRECOMPILE)
                records.erase(records.begin(), records.end() - MAX_PRECOMPILE);
            std::vector<Program*> programs;
            m_precompileStart = plat::TickCountMs();
            {
                std::lock_guard<std::mutex> lock(m_programMutex);
                for (const ProgramKey& record : records)
                    m_recorded.insert(HashBytes(&record, sizeof(record)));
            }
            for (const ProgramKey& record : records)
                programs.push_back(FindProgram(record, false));
            m_precompileTotal += uint32_t(programs.size());
            Log("[vulkan] %zu pipelines of earlier runs queued for %u compile workers", programs.size(), m_workerCount);
            for (Program* program : programs)
            {
                Enqueue(false, [this, program] {
                    const ProgramKey& key = program->key;
                    bool shaders = LoadCachedSpirv(key.vs, Stage::Vertex) && (!key.gs || LoadCachedSpirv(key.gs, Stage::Geometry)) &&
                                   (!key.ps || LoadCachedSpirv(key.ps, Stage::Pixel));
                    if (shaders && m_gpl)
                    {
                        const uint32_t parts[4] = { PART_VERTEX_INPUT, PART_PRE_RASTERIZATION, PART_FRAGMENT_SHADER, PART_FRAGMENT_OUTPUT };
                        Lazy* libraries[4];
                        Libraries(key, libraries);
                        std::array<VkPipeline, 4> handles{};
                        bool built = true;
                        for (uint32_t i = 0; i < 4 && built; i++)
                        {
                            handles[i] = Build(*libraries[i], [&] { return CreateGraphics(key, parts[i]); });
                            built = handles[i] != VK_NULL_HANDLE;
                        }
                        if (built)
                            Build(program->full, [&] { return CreateGraphics(key, 0, handles.data(), true); });
                    }
                    else if (shaders)
                    {
                        Build(program->full, [&] { return CreateGraphics(key, ALL_PARTS); });
                    }
                    PrecompileDone();
                });
            }
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
            int64_t start, end;
            start = int64_t(plat::PerfCounter());
            bool created = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &create, nullptr, &pipeline->pipeline), desc.name.c_str());
            end = int64_t(plat::PerfCounter());
            m_timing.pipelines++;
            m_timing.pipelineTicks += uint64_t(end - start);
            if (!created)
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
            // The effect constants first: waiting for room in the upload ring may submit, which
            // would end a rendering instance begun before (the draw recorded outside of it).
            if (!m_drawState.effectsValid || memcmp(&m_drawState.effects, &draw.effects, sizeof(draw.effects)) != 0)
            {
                Upload upload = AllocateUpload(sizeof(draw.effects), 256);
                if (!upload.cpu)
                    return;
                memcpy(upload.cpu, &draw.effects, sizeof(draw.effects));
                m_drawState.effects = draw.effects;
                m_drawState.effectsOffset = upload.offset;
                m_drawState.effectsValid = true;
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

            // The optimized pipeline once a worker has linked it, the fast-linked one until then.
            VkPipeline handle = pipeline->program->Current();
            if (!handle)
                return;
            if (state.pipeline != handle)
            {
                vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, handle);
                state.pipeline = handle;
            }
            SetViewportScissor(draw.viewport, draw.scissor);
            SetDrawDynamic(pipeline->dynamic);
            if (m_profiling && m_profileDraws)
                ProfileStamp("draw", nullptr);
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
            // The constants are held until the pass is recorded (a full descriptor pool submits in between).
            m_uploadRing.Hold();
            struct Release
            {
                Ring& ring;
                ~Release() { ring.Release(); }
            } release{ m_uploadRing };
            Upload constants{};
            if (pass.constantBytes)
            {
                constants = AllocateUpload(AlignUp64(pass.constantBytes, 256), 256);
                if (!constants.cpu)
                    return;
                memcpy(constants.cpu, pass.constants, pass.constantBytes);
            }
            m_profileOperation = pipeline->name.c_str();
            BeginRendering(targets, pass.targetCount, nullptr);
            m_profileOperation = nullptr;

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

        // VK_SUBOPTIMAL_KHR: the swap chain is made again, but only twice in a row. A driver that
        // keeps answering it for a swap chain of the window's size would otherwise get a new swap
        // chain (a GPU wait and new images) every frame.
        void VulkanDevice::NoteSuboptimal(const char* call)
        {
            m_stats.suboptimal++;
            if (m_frameSuboptimal)
                return;  // acquire and present of the same frame count once
            m_frameSuboptimal = true;
            if (m_suboptimalStreak++ < 2)
            {
                m_swapchainDirty = true;
                return;
            }
            if (m_suboptimalStreak == 3)
                Log("[vulkan] %s keeps returning VK_SUBOPTIMAL_KHR for %ux%u: the swap chain is kept", call, m_swapchainExtent.width,
                    m_swapchainExtent.height);
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
                NoteSuboptimal("vkAcquireNextImageKHR");
            else if (result != VK_SUCCESS)
            {
                Check(result, "vkAcquireNextImageKHR");
                m_stats.unshownFrames++;
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
            // Its old contents aren't needed (the present pass covers it), unless the safe mode keeps them.
            if (!(m_safe & SAFE_PRESENT))
                image->access = Access::Undefined;
            return image;
        }

        void VulkanDevice::Present(bool vsync)
        {
            if (vsync != m_vsync)
            {
                m_vsync = vsync;
                m_swapchainDirty = true;  // another present mode, from the next frame
            }
            int64_t submitStart, submitEnd, presentEnd;
            submitStart = int64_t(plat::PerfCounter());
            if (m_gpuLoad)
            {
                uint32_t constants[4] = { m_gpuLoad };
                PassDraw load;
                load.pipeline = m_gpuLoadPipeline;
                load.targets[0] = m_gpuLoadTarget;
                load.viewport = { 0.0f, 0.0f, 64.0f, 64.0f };
                load.scissor = { 0, 0, 64, 64 };
                load.constants = constants;
                load.constantBytes = sizeof(constants);
                DrawPass(load);
            }
            uint32_t imageIndex = m_imageIndex;
            if (imageIndex != UINT32_MAX)
                Transition(m_swapchainImages[imageIndex].get(), Access::Present);
            SubmitInternal(imageIndex != UINT32_MAX);
            submitEnd = int64_t(plat::PerfCounter());
            if (imageIndex != UINT32_MAX)
            {
                VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores = &m_renderFinished[imageIndex];
                present.swapchainCount = 1;
                present.pSwapchains = &m_swapchain;
                present.pImageIndices = &imageIndex;
                VkResult result = vkQueuePresentKHR(m_queue, &present);
                if (result == VK_ERROR_OUT_OF_DATE_KHR)
                    m_swapchainDirty = true;
                else if (result == VK_SUBOPTIMAL_KHR)
                    NoteSuboptimal("vkQueuePresentKHR");
                else
                    Check(result, "vkQueuePresentKHR");
                m_imageIndex = UINT32_MAX;
                if (!m_frameSuboptimal)
                    m_suboptimalStreak = 0;
                m_frameSuboptimal = false;
            }
            presentEnd = int64_t(plat::PerfCounter());
            m_timing.submitTicks += uint64_t(submitEnd - submitStart);
            m_timing.presentTicks = std::max(m_timing.presentTicks, uint64_t(presentEnd - submitEnd));

            m_frameValues.push_back(m_timelineValue);
            m_presents++;
            m_profiling = m_profilePool && m_presents + 1 >= m_profileFirst && m_presents + 1 <= m_profileLast;
            int64_t waitStart, waitEnd;
            waitStart = int64_t(plat::PerfCounter());
            // Two frames in flight; the safe mode's "serial" waits for this one.
            while (m_frameValues.size() > ((m_safe & SAFE_SERIAL) ? 0u : 2u))
            {
                WaitValue(m_frameValues.front());
                m_frameValues.pop_front();
            }
            waitEnd = int64_t(plat::PerfCounter());
            m_timing.frameWaitTicks += uint64_t(waitEnd - waitStart);
            if (m_unsavedPipelines && plat::TickCountMs() - m_lastCacheSave > 10000)
                SavePipelineCache();
        }

        void VulkanDevice::SavePipelineCache()
        {
            m_lastCacheSave = plat::TickCountMs();
            m_unsavedPipelines = 0;
            if (m_shaderCache.empty() || !m_pipelineCache)
                return;
            // The keys of the pipelines first drawn with since the last save (StartPrecompile).
            std::vector<ProgramKey> records;
            {
                std::lock_guard<std::mutex> lock(m_programMutex);
                records.swap(m_newRecords);
            }
            if (!records.empty())
            {
                std::filesystem::path path = m_shaderCache / "pipeline_keys.bin";
                std::error_code error;
                bool fresh = std::filesystem::file_size(path, error) < 8 || error;
                std::ofstream file(path, std::ios::binary | (fresh ? std::ios::trunc : std::ios::app));
                if (fresh)
                {
                    const uint32_t header[2] = { RECORDS_MAGIC, RECORDS_VERSION };
                    file.write(reinterpret_cast<const char*>(header), sizeof(header));
                }
                file.write(reinterpret_cast<const char*>(records.data()), std::streamsize(records.size() * sizeof(ProgramKey)));
            }
            size_t size = 0;
            if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || !size)
                return;
            // The workers may add to the cache between both calls: VK_INCOMPLETE still gives whole entries.
            std::vector<char> data(size);
            VkResult result = vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
                return;
            // Written next to it and renamed: a game closed in the middle doesn't leave half a cache.
            std::filesystem::path path = m_shaderCache / "vulkan_pipelines.bin";
            std::filesystem::path temporary = m_shaderCache / "vulkan_pipelines.bin.tmp";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                file.write(data.data(), std::streamsize(size));
                if (!file)
                    return;
            }
            std::error_code error;
            std::filesystem::rename(temporary, path, error);
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

        void VulkanDevice::Probe(Texture* texture, const std::string& tag)
        {
            auto* source = static_cast<VulkanTexture*>(texture);
            if (!source || source->depth || (source->format != VK_FORMAT_R8G8B8A8_UNORM && source->format != VK_FORMAT_B8G8R8A8_UNORM &&
                                             source->format != VK_FORMAT_B8G8R8A8_SRGB && source->format != VK_FORMAT_R8G8B8A8_SRGB))
                return;
            if (!m_probeBuffer && !(m_probeBuffer = CreateBuffer(uint64_t(PROBE_SLOTS) * PROBE_TEXELS * 4, true)))
                return;
            if (m_probes.size() >= PROBE_SLOTS)
                return;
            uint32_t slot = m_probeNext++ % PROBE_SLOTS;
            Access before = source->access;
            m_passId++;
            Transition(source, Access::TransferSrc);
            FlushBarriers();
            EndRendering();
            VkBufferImageCopy regions[PROBE_TEXELS]{};
            for (uint32_t i = 0; i < PROBE_TEXELS; i++)
            {
                regions[i].bufferOffset = (uint64_t(slot) * PROBE_TEXELS + i) * 4;
                regions[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                regions[i].imageOffset = { int32_t((2 * (i % 8) + 1) * source->desc.width / 16), int32_t((2 * (i / 8) + 1) * source->desc.height / 16), 0 };
                regions[i].imageExtent = { 1, 1, 1 };
            }
            vkCmdCopyImageToBuffer(m_cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(m_probeBuffer)->buffer,
                                   PROBE_TEXELS, regions);
            VkBufferMemoryBarrier2 hostRead{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
            hostRead.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            hostRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            hostRead.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            hostRead.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostRead.buffer = static_cast<VulkanBuffer*>(m_probeBuffer)->buffer;
            hostRead.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &hostRead;
            vkCmdPipelineBarrier2(m_cmd, &dependency);
            if (before != Access::Undefined)
                Transition(source, before);
            bool bgra = source->format == VK_FORMAT_B8G8R8A8_UNORM || source->format == VK_FORMAT_B8G8R8A8_SRGB;
            m_probes.push_back({ m_timelineValue + 1, slot, bgra, tag });
        }

        void VulkanDevice::CollectProbes(uint64_t completed)
        {
            while (!m_probes.empty() && m_probes.front().value <= completed)
            {
                const ProbeRecord& probe = m_probes.front();
                const uint8_t* texels = m_probeBuffer->cpu + uint64_t(probe.slot) * PROBE_TEXELS * 4;
                double sum[3] = {};
                uint32_t black = 0;
                for (uint32_t i = 0; i < PROBE_TEXELS; i++)
                {
                    const uint8_t* t = texels + i * 4;
                    for (int c = 0; c < 3; c++)
                        sum[c] += t[c];
                    if (std::max({ t[0], t[1], t[2] }) < 8)
                        black++;
                }
                int r = probe.bgra ? 2 : 0, b = probe.bgra ? 0 : 2;
                Log("[probe] %s: mean %.0f %.0f %.0f, black %u/%u", probe.tag.c_str(), sum[r] / PROBE_TEXELS, sum[1] / PROBE_TEXELS,
                    sum[b] / PROBE_TEXELS, black, PROBE_TEXELS);
                m_probes.pop_front();
            }
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

        // Profile ----------------------------------------------------------------------------------

        // Queries are reset in blocks outside rendering instances, ahead of their use (the timestamps
        // of CRASH_GPU_PROFILE_DRAWS are written inside them); UINT32_MAX: none free.
        uint32_t VulkanDevice::ProfileQuery()
        {
            if (m_profileReset - m_profileNext < PROFILE_BLOCK / 4 && !m_rendering.active &&
                m_profileRecords.size() + (m_profileReset - m_profileNext) + PROFILE_BLOCK <= PROFILE_QUERIES)
            {
                vkCmdResetQueryPool(m_cmd, m_profilePool, uint32_t(m_profileReset % PROFILE_QUERIES), PROFILE_BLOCK);
                m_profileReset += PROFILE_BLOCK;
            }
            if (m_profileNext == m_profileReset)
                return UINT32_MAX;  // the GPU is far behind: this segment joins the last one
            return uint32_t(m_profileNext++ % PROFILE_QUERIES);
        }

        void VulkanDevice::ProfileStamp(const char* operation, const VulkanTexture* target)
        {
            uint32_t query = ProfileQuery();
            if (query == UINT32_MAX)
                return;
            char text[160];
            if (target)
                snprintf(text, sizeof(text), " | %s -> %s %ux%u", operation, target->desc.name ? target->desc.name : "?", target->desc.width,
                         target->desc.height);
            else
                snprintf(text, sizeof(text), " | %s", operation);
            std::string name = m_profileLabel + text;
            auto [it, inserted] = m_profileIndex.try_emplace(name, uint32_t(m_profileEntries.size()));
            if (inserted)
                m_profileEntries.push_back({ name });
            vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_profilePool, query);
            m_profileRecords.push_back({ m_timelineValue + 1, query, it->second, m_stats.barriers });
        }

        void VulkanDevice::ProfileEnd()
        {
            if (m_profileRecords.empty() || m_profileRecords.back().entry == PROFILE_END || m_profileRecords.back().value != m_timelineValue + 1)
                return;
            uint32_t query = ProfileQuery();
            if (query == UINT32_MAX)
            {
                // No query left for the end: the command buffer's last segment is dropped.
                m_profileRecords.back().entry = PROFILE_END;
                return;
            }
            vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_profilePool, query);
            m_profileRecords.push_back({ m_timelineValue + 1, query, PROFILE_END, m_stats.barriers });
        }

        // A record's segment lasts until the next record, of the same command buffer (each one ends
        // with a PROFILE_END record).
        void VulkanDevice::CollectProfile(uint64_t completed)
        {
            while (m_profileRecords.size() >= 2 && m_profileRecords.front().value <= completed)
            {
                ProfileRecord record = m_profileRecords.front();
                m_profileRecords.pop_front();
                if (record.entry == PROFILE_END)
                    continue;
                const ProfileRecord& next = m_profileRecords.front();
                uint64_t start = 0, end = 0;
                if (vkGetQueryPoolResults(m_device, m_profilePool, record.query, 1, 8, &start, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
                    vkGetQueryPoolResults(m_device, m_profilePool, next.query, 1, 8, &end, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                    continue;
                ProfileEntry& entry = m_profileEntries[record.entry];
                entry.ticks += (end - start) & m_timestampMask;
                entry.count++;
                entry.barriers += next.barriers - record.barriers;
            }
            if (m_profileRecords.size() == 1 && m_profileRecords.front().entry == PROFILE_END && m_profileRecords.front().value <= completed)
                m_profileRecords.pop_front();
            if (!m_profileLogged && m_presents >= m_profileLast && m_profileRecords.empty() && !m_profileEntries.empty())
                LogProfile();
        }

        void VulkanDevice::LogProfile()
        {
            m_profileLogged = true;
            double frames = double(m_profileLast - m_profileFirst + 1);
            double msPerTick = 1000.0 / m_timestampFrequency;
            std::vector<const ProfileEntry*> entries;
            uint64_t ticks = 0, barriers = 0, count = 0;
            for (const ProfileEntry& entry : m_profileEntries)
            {
                entries.push_back(&entry);
                ticks += entry.ticks;
                barriers += entry.barriers;
                count += entry.count;
            }
            std::sort(entries.begin(), entries.end(), [](const ProfileEntry* a, const ProfileEntry* b) { return a->ticks > b->ticks; });
            Log("[gpu profile] frames %llu-%llu: GPU busy %.2f ms per frame, %.1f segments, %.1f barriers", (unsigned long long)m_profileFirst,
                (unsigned long long)m_profileLast, double(ticks) * msPerTick / frames, double(count) / frames, double(barriers) / frames);
            Log("[gpu profile] ms/frame      %%   n/frame  barriers/frame  label | operation -> target");
            for (const ProfileEntry* entry : entries)
                Log("[gpu profile] %8.3f %6.1f %9.1f %15.1f  %s", double(entry->ticks) * msPerTick / frames,
                    ticks ? 100.0 * double(entry->ticks) / double(ticks) : 0.0, double(entry->count) / frames, double(entry->barriers) / frames,
                    entry->name.c_str());
        }

        // Native access ----------------------------------------------------------------------------

        NativeHandles VulkanDevice::Native()
        {
            EndRendering();
            if (m_profiling)
                ProfileStamp("native", nullptr);
            FlushBarriers();
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
            Log("[gpu] vulkan pipelines (%s%s): %llu whole, %llu libraries, %llu fast links, %llu optimized, %llu failed; %llu draws waited for a compile, "
                "%llu skipped while compiling; %u/%u start-up compiles",
                m_compileMode == ShaderCompilation::Sync ? "sync" : m_compileMode == ShaderCompilation::Aggressive ? "aggressive" : "async",
                m_gpl ? ", libraries" : "", (unsigned long long)m_compileStats.programs, (unsigned long long)m_compileStats.libraries,
                (unsigned long long)m_compileStats.fastLinks, (unsigned long long)m_compileStats.optimized, (unsigned long long)m_compileStats.failures,
                (unsigned long long)m_compileStats.stalls, (unsigned long long)m_compileStats.deferred, m_precompileDone.load(), m_precompileTotal.load());
            const uint64_t* r = m_stats.renderingReasons;
            Log("[gpu] vulkan: rendering instances begun because none was open %llu, color count %llu, color targets %llu, depth %llu, barriers %llu",
                (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4]);
            if (m_debug)
                Log("[gpu] vulkan: %llu validation message(s)", (unsigned long long)g_validationMessages);
            std::string budget;
            if (m_memoryBudget)
            {
                VkPhysicalDeviceMemoryBudgetPropertiesEXT heaps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
                VkPhysicalDeviceMemoryProperties2 memory{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &heaps };
                vkGetPhysicalDeviceMemoryProperties2(m_physical, &memory);
                for (uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; i++)
                {
                    char text[64];
                    snprintf(text, sizeof(text), ", heap %u %llu/%llu MB", i, (unsigned long long)(heaps.heapUsage[i] >> 20),
                             (unsigned long long)(heaps.heapBudget[i] >> 20));
                    budget += text;
                }
            }
            Log("[gpu] vulkan: %llu swap chains, %llu suboptimal, %llu frames not shown; upload ring %llu MB, peak %llu MB, %llu waits (%llu mid-frame submits)%s",
                (unsigned long long)m_stats.swapchains, (unsigned long long)m_stats.suboptimal, (unsigned long long)m_stats.unshownFrames,
                (unsigned long long)(m_uploadSize >> 20), (unsigned long long)(m_stats.ringPeak >> 20), (unsigned long long)m_stats.ringWaits,
                (unsigned long long)m_stats.ringSubmits, budget.c_str());
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
