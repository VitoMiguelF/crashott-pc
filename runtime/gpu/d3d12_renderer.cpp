// Direct3D 12 renderer: turns the Xenos register state at each draw into D3D12 work.
//
// Design (first version, favouring simplicity over speed and accuracy):
// - EDRAM is not emulated as memory. Each (base tile, row width in samples, format) used as a
//   color or depth render target gets its own host render target (no host MSAA, 2048 rows): 1x
//   and 2x surfaces map one pixel to one host pixel, 4x surfaces (half the pitch for the same
//   EDRAM) two host pixels along x. Data shared between other EDRAM views is not transferred,
//   except for D3D's depth-alias color clears (DrawImpl).
// - Resolves (draws with RB_MODECONTROL = copy) copy the render target rectangle into a host
//   texture registered under the destination guest address ("resolve targets"); guest memory is
//   not written. Texture fetches and VdSwap look those up first. Clears requested by the resolve
//   are done with ClearRenderTargetView/ClearDepthStencilView.
// - Other textures are decoded on the CPU from guest memory (base level only) and re-uploaded when
//   the memory hash changes.
// - Vertex data is fetched in the shader from a per-draw raw buffer holding the byte-swapped
//   range of each vertex fetch constant used; indices are expanded to 32 bits on the CPU (offset,
//   clamp, primitive reset, fans/quads converted to lists; rectangle lists go through a GS).
// - The viewport transform is done in the vertex shader against a host viewport covering the
//   whole render target, as Xenia does for draws without clipping (draw_util.cc).
//
// Register meanings and the resolve rectangle rules follow Xenia (BSD license).

#include "renderer.h"
#include "gpu.h"
#include "smaa.h"
#include "upscaler.h"
#include "texture_watch.h"
#include "graphics_options.h"
#include "hd_textures.h"
#include "lighting.h"
#include "overlay.h"
#include "png_writer.h"
#include "texture_decode.h"
#include "window.h"
#include "xenos.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../kernel/input.h"
#include "../kernel/memory.h"
#include "../settings/direct_boot.h"
#include "../settings/prompt_icons.h"
#include "../settings/quit_game.h"
#include "../settings/settings.h"
#include "../settings/save_screens.h"
#include "../settings/skip_cinematics.h"
#include "../settings/settings_menu.h"
#include "../subtitles/subtitle_draw.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <emmintrin.h>

using Microsoft::WRL::ComPtr;

namespace gpu::renderer
{
    namespace
    {
        constexpr uint32_t WINDOW_WIDTH = 1280;
        constexpr uint32_t WINDOW_HEIGHT = 720;
        constexpr uint32_t BACK_BUFFER_COUNT = 2;
        constexpr uint64_t UPLOAD_RING_SIZE = 256ull << 20;
        constexpr uint32_t SHADER_VISIBLE_DESCRIPTORS = 65536;
        constexpr uint32_t VERTEX_DATA_DESCRIPTOR = SHADER_VISIBLE_DESCRIPTORS - 1;  // raw SRV of the whole upload ring
        constexpr uint32_t SAMPLER_DESCRIPTORS = 2048;
        constexpr uint32_t CPU_SRV_DESCRIPTORS = 32768;
        constexpr uint32_t RTV_DESCRIPTORS = 4096;
        constexpr uint32_t DSV_DESCRIPTORS = 1024;
        constexpr uint32_t RENDER_TARGET_HEIGHT = 2048;

        // Internal resolution (settings video.resolution): host render targets and resolve targets
        // are this many times larger than the guest's. Their logical sizes (RenderTarget::width,
        // ResolveTarget::width...) stay in guest units, so EDRAM and resolve matching don't change;
        // resources, viewports, scissors, clear rectangles and resolve offsets are scaled.
        float g_renderScale = 1.0f;
        // Scale of the 3D scene (settings::SceneScale): below g_renderScale when a temporal
        // upscaler (upscaler.h) draws the scene smaller and upscales it. The scene is what the game
        // draws into its MSAA render targets (the predicated-tiling passes; 2D, the post-processing
        // and the HUD use 1x targets), so render targets first used with 2x/4x MSAA get this scale,
        // and resolve targets get one surface per scale they are resolved at.
        float g_sceneScale = 1.0f;

        uint32_t ScaledSize(uint32_t value, float scale) { return uint32_t(ceilf(float(value) * scale)); }
        LONG ScaledFloor(float value, float scale) { return LONG(floorf(value * scale + 0.001f)); }
        LONG ScaledCeil(float value, float scale) { return LONG(ceilf(value * scale - 0.001f)); }
        constexpr uint32_t MAX_VERTEX_RANGE_BYTES = 32u << 20;

        // Root signature parameters of the draw root signature.
        enum RootParameter : uint32_t
        {
            ROOT_SYSTEM_CONSTANTS = 0,  // b0
            ROOT_VS_CONSTANTS = 1,      // b1
            ROOT_PS_CONSTANTS = 2,      // b2
            ROOT_TEXTURES = 3,          // t0-t31
            ROOT_VERTEX_DATA = 4,       // t0 space1
            ROOT_SAMPLERS = 5,          // s0-s31
            ROOT_EFFECTS = 6,           // b3: graphics option constants (graphics_options.h)
            ROOT_PARAMETER_COUNT,
        };

        // Must match xe_system in shader_translator.cpp.
        struct SystemConstants
        {
            float ndcScale[4];
            float ndcOffset[4];
            uint32_t flags;
            uint32_t alphaFunc;
            uint32_t paramGenRegister;
            uint32_t padding0;
            float alphaRef[4];
            float colorExpBias[4];
            uint32_t boolConsts[8];
            uint32_t loopConsts[32];
            int32_t vbBases[96];
            uint32_t texSwizzles[32];
            float texLodBias[32];
            // Read only by shaders translated at an internal resolution above 720p.
            float renderScale[4];      // x: guest pixels per host pixel
            float texSizeScale[32];    // guest texels per host texel
        };

        enum SystemFlags : uint32_t
        {
            FLAG_XY_DIVIDED_BY_W = 1,
            FLAG_Z_DIVIDED_BY_W = 2,
            FLAG_W_NOT_RECIPROCAL = 4,
            FLAG_PARAM_GEN = 8,
            FLAG_ALPHA_TEST = 16,
        };

        // Registers past the write-trapped first page are read straight from the big-endian MMIO
        // window (the renderer reads thousands per frame; this keeps it inline).
        constexpr uint32_t UNTRAPPED_REGISTERS_BEGIN = 0x1000 / 4;

        uint32_t Reg(uint32_t index)
        {
            if (index >= UNTRAPPED_REGISTERS_BEGIN && index < gpu::REGISTER_COUNT)
                return LoadBE32(GPU_MMIO_BEGIN + index * 4);
            return gpu::ReadRegister(index);
        }

        float RegF(uint32_t index)
        {
            uint32_t value = gpu::ReadRegister(index);
            float result;
            memcpy(&result, &value, 4);
            return result;
        }

        uint64_t AlignUp64(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0)
        {
            // Four independent multiply-xor lanes; fast enough to hash textures every frame.
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            constexpr uint64_t P = 0x9E3779B97F4A7C15ull;
            uint64_t h[4] = { seed ^ 0x243F6A8885A308D3ull, seed ^ 0x13198A2E03707344ull, seed ^ 0xA4093822299F31D0ull, seed ^ 0x082EFA98EC4E6C89ull };
            size_t i = 0;
            for (; i + 32 <= size; i += 32)
            {
                uint64_t v[4];
                memcpy(v, bytes + i, 32);
                for (int lane = 0; lane < 4; lane++)
                {
                    h[lane] ^= v[lane];
                    h[lane] *= P;
                    h[lane] ^= h[lane] >> 29;
                }
            }
            uint64_t result = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ size;
            for (; i < size; i++)
                result = (result ^ bytes[i]) * 0x100000001B3ull;
            result ^= result >> 31;
            result *= P;
            return result ^ (result >> 29);
        }

        // Content hash of guest textures, checked once per frame for every texture a frame uses
        // (~25 MB per frame in the village). XXH3's accumulation on SSE2 lanes: per 16 bytes, a
        // 32x32->64 multiply of the input (xor a key) by its own swapped halves, plus the input,
        // into 64-bit accumulators. ~2.5x the speed of HashBytes, whose 64-bit multiply chain
        // limited it to ~7.5 GB/s (tools/perf, runtime/NOTES.md "Desempenho"). Only compared with
        // itself (TextureEntry::contentHash), so it never has to match HashBytes.
        uint64_t HashContent(const void* data, size_t size, uint64_t seed = 0)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            __m128i acc[4] = { _mm_set1_epi64x(int64_t(seed ^ 0x243F6A8885A308D3ull)), _mm_set1_epi64x(int64_t(seed ^ 0x13198A2E03707344ull)),
                               _mm_set1_epi64x(int64_t(seed ^ 0xA4093822299F31D0ull)), _mm_set1_epi64x(int64_t(seed ^ 0x082EFA98EC4E6C89ull)) };
            const __m128i key[4] = { _mm_set_epi64x(int64_t(0x7C01812CF721AD1Cull), int64_t(0xBE4BA423396CFEB8ull)),
                                     _mm_set_epi64x(int64_t(0xDED46DE9839097DBull), int64_t(0x1CAD21F72C81017Cull)),
                                     _mm_set_epi64x(int64_t(0x78E5C0CC4EE679CBull), int64_t(0x2172FFCC7DD05A82ull)),
                                     _mm_set_epi64x(int64_t(0x8E2443F7744608B8ull), int64_t(0x4C263A81E69035E0ull)) };
            size_t i = 0;
            for (; i + 64 <= size; i += 64)
            {
                for (int k = 0; k < 4; k++)
                {
                    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + i + 16 * k));
                    __m128i x = _mm_xor_si128(v, key[k]);
                    __m128i product = _mm_mul_epu32(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)));
                    acc[k] = _mm_add_epi64(acc[k], _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
                    acc[k] = _mm_add_epi64(acc[k], product);
                }
            }
            alignas(16) uint64_t lanes[8];
            for (int k = 0; k < 4; k++)
                _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 2 * k), acc[k]);
            constexpr uint64_t P = 0x9E3779B97F4A7C15ull;
            uint64_t result = size;
            for (int k = 0; k < 8; k++)
            {
                result ^= lanes[k];
                result *= P;
                result ^= result >> 29;
            }
            for (; i < size; i++)
                result = (result ^ bytes[i]) * 0x100000001B3ull;
            result ^= result >> 31;
            result *= P;
            return result ^ (result >> 29);
        }

        // Ring allocator with fence-based reclamation (upload memory, descriptors).
        class Ring
        {
        public:
            void Init(uint64_t capacity) { m_capacity = capacity; }

            // Returns the offset, or UINT64_MAX if there's no contiguous room right now.
            uint64_t TryAllocate(uint64_t size, uint64_t alignment)
            {
                if (size > m_capacity)
                    return UINT64_MAX;
                uint64_t used = m_allocated - m_freed;
                if (used == 0)
                    m_head = m_tail = 0;
                if (m_head >= m_tail && used < m_capacity)
                {
                    uint64_t aligned = AlignUp64(m_head, alignment);
                    if (aligned + size <= m_capacity)
                        return Take(aligned, size);
                    // Wrap: the end of the buffer is wasted.
                    if (size <= m_tail || used == 0)
                    {
                        m_allocated += m_capacity - m_head;
                        m_head = 0;
                        return Take(0, size);
                    }
                    return UINT64_MAX;
                }
                if (m_head < m_tail)
                {
                    uint64_t aligned = AlignUp64(m_head, alignment);
                    if (aligned + size <= m_tail)
                        return Take(aligned, size);
                }
                return UINT64_MAX;
            }

            void OnSubmit(uint64_t fence) { m_marks.push_back({ fence, m_head, m_allocated }); }

            void Retire(uint64_t completedFence)
            {
                while (!m_marks.empty() && m_marks.front().fence <= completedFence)
                {
                    m_tail = m_marks.front().position;
                    m_freed = m_marks.front().allocated;
                    m_marks.pop_front();
                }
            }

            bool HasPending() const { return !m_marks.empty(); }
            uint64_t OldestPendingFence() const { return m_marks.front().fence; }

        private:
            struct Mark
            {
                uint64_t fence;
                uint64_t position;
                uint64_t allocated;
            };

            uint64_t Take(uint64_t offset, uint64_t size)
            {
                m_allocated += offset + size - m_head;
                m_head = offset + size;
                if (m_head == m_capacity)
                    m_head = 0;
                return offset;
            }

            uint64_t m_capacity = 0;
            uint64_t m_head = 0;
            uint64_t m_tail = 0;
            uint64_t m_allocated = 0;
            uint64_t m_freed = 0;
            std::deque<Mark> m_marks;
        };

        struct TrackedResource
        {
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        };

        struct ShaderEntry
        {
            TranslatedShader info;
            std::vector<uint32_t> ucode;
            ComPtr<ID3DBlob> bytecode;
            bool translated = false;
            bool failed = false;
        };

        struct RenderTarget
        {
            TrackedResource texture;
            DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;  // or DSV format for depth
            DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
            D3D12_CPU_DESCRIPTOR_HANDLE view{};           // RTV or DSV
            uint32_t srvIndex = 0;
            uint32_t stencilSrvIndex = 0;                 // depth only: the stencil plane (uint, in .y)
            uint32_t width = 0, height = 0;            // guest units (host pixels at 1x)
            uint32_t hostWidth = 0, hostHeight = 0;    // the resource (scaled)
            uint32_t guestFormat = 0;
            bool isDepth = false;
            // EDRAM sample rows each host row stands for: 1 for a target first used at 1x, 2 for
            // one first used at 2x/4x (1x and 2x share the pixel grid; see SamplesX).
            uint32_t rowSamples = 1;
            float scale = 1.0f;   // host pixels per guest pixel: g_sceneScale for MSAA targets
            bool IsScene() const { return rowSamples == 2; }
        };

        // The host texture of a resolve target at one scale.
        struct ResolveSurface
        {
            TrackedResource texture;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            uint32_t srvIndex = 0;
            uint32_t hostWidth = 0, hostHeight = 0;  // the resource (scaled)
            float scale = 0.0f;                      // 0: not created

            // Depth resolves only: the 32-bit words the console writes to memory, (depth24 << 8) |
            // stencil, as RGBA8 bytes (R = stencil). Fetches with a non-depth format (the game reads
            // the stencil of a depth resolve as k_8_8_8_8 for its stencil shadows) sample this one;
            // k_24_8/k_24_8_FLOAT fetches sample the float depth in `texture`.
            TrackedResource packed;
            D3D12_CPU_DESCRIPTOR_HANDLE packedRtv{};
            uint32_t packedSrvIndex = 0;
        };

        struct ResolveTarget
        {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            uint32_t width = 0, height = 0;   // guest texels (width = aligned pitch)
            uint32_t bytesPerPixel = 4;
            uint32_t guestFormat = 0;
            uint64_t lastWriteFrame = 0;
            // [0] at g_renderScale, [1] at g_sceneScale (only with an upscaler drawing the scene
            // smaller). Resolves write the surface of their source's scale; fetches and VdSwap read
            // the one written last (`current`), so the scene's resolves stay small for the scene's
            // own passes, and the upscaler's output replaces the scene color for the 2D passes.
            ResolveSurface surfaces[2];
            uint32_t current = 0;
            ResolveSurface& Current() { return surfaces[current]; }
        };

        // A replacement from an HD texture pack (hd_textures.h), shared by every guest texture
        // with its key; created on first use and kept.
        struct HdTexture
        {
            TrackedResource texture;
            uint32_t srvIndex = 0;
            uint32_t width = 0, height = 0;
            bool failed = false;
        };

        struct TextureEntry
        {
            TrackedResource texture;
            uint32_t srvIndex = 0;
            uint64_t contentHash = 0;
            uint64_t checkedFrame = UINT64_MAX;
            bool uploaded = false;
            // Write watch (gpu/texture_watch.h): checks in a row with the same content, whether its
            // pages are protected now, and how often they were written without the content changing
            // (a neighbour on the same pages): after a few of those it isn't watched any more.
            uint8_t stableChecks = 0;
            bool watched = false;
            bool watchDirtied = false;
            uint8_t watchMisses = 0;
            uint64_t lastUploadFrame = 0;  // dynamic textures (re-uploaded within a second) aren't dumped
            HdTexture* hd = nullptr;    // replacement of the current contents, if any
            float hdScale = 1.0f;       // guest texels per HD texel
            // A button icon of the game (settings/prompt_icons.h): an 8-bit index texture read
            // through a 256x1 palette. Which icon, its area, and the replacement (indices + palette)
            // drawn for `iconStyle`.
            char32_t icon = 0;
            uint64_t iconKey = 0;
            uint32_t iconWidth = 0, iconHeight = 0;
            int iconBox[4] = {};
            uint32_t iconStyle = 0;
            struct IconTexture* iconTexture = nullptr;
        };

        // A button icon drawn for the device in use: its indices and its palette.
        struct IconTexture
        {
            HdTexture indices, palette;
        };

        struct Stats
        {
            uint64_t draws = 0;
            uint64_t drawsSkipped = 0;
            uint64_t resolves = 0;
            uint64_t clears = 0;
            uint64_t aliasClears = 0;  // depth-only draws also drawn into the aliased color target
            uint64_t swaps = 0;
            uint64_t presentedFrames = 0;
            uint64_t textureUploads = 0;
            uint64_t hdTextures = 0;
            uint64_t shaderCompiles = 0;
            uint64_t shaderFailures = 0;
            uint64_t pipelines = 0;
            uint64_t submits = 0;
            uint64_t skipReasons[8] = {};
        } g_stats;

        // Work counters of the command processor thread, logged per frame with the pacing stats
        // (every 300 frames) and reset there: what the per-draw costs scale with.
        struct WorkCounters
        {
            uint64_t draws = 0;
            uint64_t indices = 0;          // guest indices read by BuildIndices
            uint64_t textureLookups = 0;   // GetTextureSrv calls for guest textures
            uint64_t textureChecks = 0;    // guest textures hashed (once per frame each)
            uint64_t textureBytes = 0;     // bytes hashed for those checks
            uint64_t textureUploads = 0;
            uint64_t textureWatched = 0;   // checks skipped: pages unwritten since protected (texture_watch.h)
            uint64_t textureProtects = 0;
        } g_work;

        // Skip reasons (CRASH_GPU_TRACE=N also logs the first N draws in detail).
        enum SkipReason
        {
            SKIP_EDRAM_MODE,
            SKIP_VIZ_QUERY,
            SKIP_SHADER,
            SKIP_PRIMITIVE,
            SKIP_NO_TARGET,
            SKIP_PIPELINE,
            SKIP_CULL,
        };
        const char* const SKIP_NAMES[] = { "edram mode", "viz query", "shader", "primitive", "no target", "pipeline", "culled" };
        uint32_t g_traceDraws = 0;
        uint64_t g_traceFrom = 0;
        uint64_t g_traceSwap = UINT64_MAX; // CRASH_GPU_TRACE_SWAP=N: every draw and resolve of the frame presented by swap N

        bool TracingSwap()
        {
            return g_stats.swaps + 1 == g_traceSwap;
        }

        void Skip(SkipReason reason)
        {
            g_stats.drawsSkipped++;
            g_stats.skipReasons[reason]++;
        }

        // Device.
        std::atomic<bool> g_active;
        bool g_debugLayer = false;
        HWND g_hwnd;
        ComPtr<IDXGIFactory4> g_factory;
        // DXGI_FEATURE_PRESENT_ALLOW_TEARING: with V-Sync off, Present(0) in a window that is not in
        // independent flip (unfocused, overlapped) is otherwise held to the monitor's refresh by DWM.
        bool g_allowTearing = false;
        ComPtr<ID3D12Device> g_device;
        ComPtr<ID3D12CommandQueue> g_queue;
        ComPtr<IDXGISwapChain3> g_swapChain;
        ComPtr<ID3D12Resource> g_backBuffers[BACK_BUFFER_COUNT];
        D3D12_CPU_DESCRIPTOR_HANDLE g_backBufferRtvs[BACK_BUFFER_COUNT];
        uint32_t g_backBufferWidth = WINDOW_WIDTH, g_backBufferHeight = WINDOW_HEIGHT;
        ComPtr<ID3D12Fence> g_fence;
        uint64_t g_fenceValue = 0;
        HANDLE g_fenceEvent;
        ComPtr<ID3D12GraphicsCommandList> g_list;
        ComPtr<ID3D12CommandAllocator> g_allocator;
        std::deque<std::pair<ComPtr<ID3D12CommandAllocator>, uint64_t>> g_allocatorPool;
        std::deque<uint64_t> g_frameFences;
        bool g_listNeedsHeaps = true;

        // Descriptor heaps.
        ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_dsvHeap, g_cpuSrvHeap, g_gpuSrvHeap, g_samplerHeap;
        uint32_t g_rtvSize, g_srvSize, g_samplerSize, g_dsvSize;
        uint32_t g_rtvCount, g_dsvCount, g_cpuSrvCount;
        uint32_t g_nullTextureSrv, g_nullBufferSrv;
        D3D12_CPU_DESCRIPTOR_HANDLE g_nullRtv;
        Ring g_descriptorRing;
        uint32_t g_samplerCount;
        std::unordered_map<uint64_t, uint32_t> g_samplerBlocks;

        // Upload ring.
        ComPtr<ID3D12Resource> g_uploadBuffer;
        uint8_t* g_uploadCpu;
        D3D12_GPU_VIRTUAL_ADDRESS g_uploadGpu;
        Ring g_uploadRing;

        // Pipelines and shaders.
        ComPtr<ID3D12RootSignature> g_drawRootSignature, g_blitRootSignature, g_blit2RootSignature;
        std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> g_pipelines;
        std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> g_shaders;
        ShaderEntry* g_currentVertexShader;
        ShaderEntry* g_currentPixelShader;
        uint64_t g_frameWaitTicks = 0; // QPC ticks Swap spent waiting for frame fences (pacing stats)
        uint64_t g_submitTicks = 0;    // ... in ExecuteCommandLists (pacing stats)
        uint64_t g_ringWaitTicks = 0;  // ... waiting for upload / descriptor ring space (pacing stats)
        uint64_t g_presentTicks = 0;   // longest Present call (pacing stats)
        // Graphics options of the current frame (from the settings, updated in Swap).
        graphics::EffectConstants g_effects;
        int g_anisotropy = 1;
        ComPtr<ID3DBlob> g_rectListGs, g_blitVs, g_resolvePs, g_resolveDepthPackedPs, g_depthAliasPs, g_presentPs;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> g_resolvePipelines; // by DXGI format
        ComPtr<ID3D12PipelineState> g_presentPipeline;
        // Post-processing anti-aliasing (settings: video.antialiasing), at the size of the front
        // buffer: the present pass first draws it 1:1 into g_aaInput (applying the fetch constant
        // swizzle), FXAA or SMAA write g_aaOutput, and a last present pass scales that to the back
        // buffer.
        ComPtr<ID3DBlob> g_fxaaPs;
        ComPtr<ID3D12PipelineState> g_fxaaPipeline;
        struct PostTarget
        {
            TrackedResource texture;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            uint32_t srvIndex = UINT32_MAX;
        };
        PostTarget g_aaInput, g_aaOutput;
        uint32_t g_aaWidth = 0, g_aaHeight = 0;
        pD3DCompile g_compile;
        HRESULT(WINAPI* g_createBlob)(SIZE_T, ID3DBlob**);
        std::filesystem::path g_shaderCacheDir;
        bool g_dumpShaders = false;

        // Caches.
        std::unordered_map<uint64_t, RenderTarget> g_renderTargets;
        std::unordered_map<uint32_t, ResolveTarget> g_resolveTargets; // by physical base address
        std::unordered_map<uint64_t, TextureEntry> g_textures;
        std::unordered_map<uint64_t, HdTexture> g_hdTextures;  // by key (hd_textures.h)
        // Button icons drawn for the device in use (settings/prompt_icons.h), by icon key and style.
        std::unordered_map<uint64_t, IconTexture> g_iconTextures;
        // The palette of the icon bound by the draw being set up (GetTextureSrv), which replaces the
        // draw's 256x1 palette texture; 0 = none.
        uint32_t g_drawIconPalette = 0;
        // CRASH_NO_PROMPT_ICONS=1 keeps the game's Xbox icons (and skips keying every texture).
        const bool g_promptIcons = getenv("CRASH_NO_PROMPT_ICONS") == nullptr;
        std::vector<uint8_t> g_hdKeyScratch;                    // decoded base level of the texture being uploaded
        std::vector<uint8_t> g_textureScratch;
        uint64_t g_frame = 0;

        // Screenshots.
        std::vector<uint64_t> g_screenshotFrames;
        std::vector<int64_t> g_screenshotTimes;  // CRASH_SCREENSHOT_TIMES, input clock ms, ascending
        std::vector<boot::Anchored> g_screenshotAnchors;  // CRASH_SCREENSHOT_TIMES entries after a test event
        std::filesystem::path g_screenshotDir;
        ComPtr<ID3D12Resource> g_readback;
        bool g_swapRedBlue = false;

        void DrainDebugMessages()
        {
            if (!g_debugLayer)
                return;
            ComPtr<ID3D12InfoQueue> queue;
            if (FAILED(g_device.As(&queue)))
                return;
            static uint32_t logged;
            uint64_t count = queue->GetNumStoredMessages();
            for (uint64_t i = 0; i < count && logged < 200; i++)
            {
                SIZE_T length = 0;
                queue->GetMessage(i, nullptr, &length);
                std::vector<uint8_t> storage(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(queue->GetMessage(i, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                {
                    Log("[d3d12] %s", message->pDescription);
                    logged++;
                }
            }
            queue->ClearStoredMessages();
        }

        // Command list state set by the last DrawImpl, so the next draw only records what changed
        // (~1,600 draws per frame in the village, ~17 calls each before). Everything else that
        // records graphics state on g_list (Resolve, RunTemporal with DLSS/FSR, the present path in
        // Swap with SMAA and the overlay, a new list in Submit) calls InvalidateDrawState().
        struct DrawListState
        {
            bool valid = false;
            ID3D12RootSignature* rootSignature = nullptr;
            ID3D12PipelineState* pipeline = nullptr;
            uint32_t renderTargetCount = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {};
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
            float blendFactor[4] = {};
            uint32_t stencilRef = 0;
            D3D12_VIEWPORT viewport = {};
            D3D12_RECT scissor = {};
            D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            uint64_t samplerTable = 0;
            graphics::EffectConstants effects;
        } g_drawState;

        void InvalidateDrawState()
        {
            g_drawState.valid = false;
        }

        // Command submission --------------------------------------------------------------------

        void WaitForFence(uint64_t value)
        {
            if (g_fence->GetCompletedValue() >= value)
                return;
            g_fence->SetEventOnCompletion(value, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, INFINITE);
        }

        void RetireCompleted()
        {
            uint64_t completed = g_fence->GetCompletedValue();
            g_uploadRing.Retire(completed);
            g_descriptorRing.Retire(completed);
        }

        void Submit()
        {
            g_list->Close();
            ID3D12CommandList* lists[] = { g_list.Get() };
            g_queue->ExecuteCommandLists(1, lists);
            g_fenceValue++;
            g_queue->Signal(g_fence.Get(), g_fenceValue);
            g_uploadRing.OnSubmit(g_fenceValue);
            g_descriptorRing.OnSubmit(g_fenceValue);
            g_allocatorPool.push_back({ g_allocator, g_fenceValue });
            g_stats.submits++;

            g_allocator.Reset();
            uint64_t completed = g_fence->GetCompletedValue();
            if (!g_allocatorPool.empty() && g_allocatorPool.front().second <= completed)
            {
                g_allocator = g_allocatorPool.front().first;
                g_allocatorPool.pop_front();
                g_allocator->Reset();
            }
            else
            {
                g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator));
            }
            g_list->Reset(g_allocator.Get(), nullptr);
            g_listNeedsHeaps = true;
            InvalidateDrawState();
            DrainDebugMessages();
        }

        void WaitIdle()
        {
            Submit();
            WaitForFence(g_fenceValue);
            RetireCompleted();
        }

        // Upload memory: returns the offset in the ring (CPU pointer = g_uploadCpu + offset).
        uint64_t AllocateUpload(uint64_t size, uint64_t alignment)
        {
            // Fence values are only queried when the ring is full (GetCompletedValue was called
            // ~10,000 times per frame, once per allocation).
            uint64_t offset = g_uploadRing.TryAllocate(size, alignment);
            if (offset != UINT64_MAX)
                return offset;
            while (true)
            {
                RetireCompleted();
                offset = g_uploadRing.TryAllocate(size, alignment);
                if (offset != UINT64_MAX)
                    return offset;
                if (size > UPLOAD_RING_SIZE)
                    return UINT64_MAX;
                LARGE_INTEGER waitStart, waitEnd;
                QueryPerformanceCounter(&waitStart);
                if (g_uploadRing.HasPending())
                    WaitForFence(g_uploadRing.OldestPendingFence());
                else
                    WaitIdle();
                QueryPerformanceCounter(&waitEnd);
                g_ringWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
            }
        }

        // Shader-visible CBV/SRV/UAV descriptors for this draw.
        uint32_t AllocateDescriptors(uint32_t count)
        {
            uint64_t offset = g_descriptorRing.TryAllocate(count, 1);
            if (offset != UINT64_MAX)
                return uint32_t(offset);
            while (true)
            {
                RetireCompleted();
                offset = g_descriptorRing.TryAllocate(count, 1);
                if (offset != UINT64_MAX)
                    return uint32_t(offset);
                LARGE_INTEGER waitStart, waitEnd;
                QueryPerformanceCounter(&waitStart);
                if (g_descriptorRing.HasPending())
                    WaitForFence(g_descriptorRing.OldestPendingFence());
                else
                    WaitIdle();
                QueryPerformanceCounter(&waitEnd);
                g_ringWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(uint32_t index)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_cpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(index) * g_srvSize;
            return handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GpuHeapCpu(uint32_t index)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_gpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(index) * g_srvSize;
            return handle;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE GpuHeapGpu(uint32_t index)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE handle = g_gpuSrvHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += uint64_t(index) * g_srvSize;
            return handle;
        }

        uint32_t AllocateCpuSrv()
        {
            if (g_cpuSrvCount >= CPU_SRV_DESCRIPTORS)
                Fatal("[gpu] out of CPU SRV descriptors");
            return g_cpuSrvCount++;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRtv()
        {
            if (g_rtvCount >= RTV_DESCRIPTORS)
                Fatal("[gpu] out of RTV descriptors");
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(g_rtvCount++) * g_rtvSize;
            return handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDsv()
        {
            if (g_dsvCount >= DSV_DESCRIPTORS)
                Fatal("[gpu] out of DSV descriptors");
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(g_dsvCount++) * g_dsvSize;
            return handle;
        }

        void Transition(TrackedResource& resource, D3D12_RESOURCE_STATES state)
        {
            if (resource.state == state)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource.resource.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = resource.state;
            barrier.Transition.StateAfter = state;
            g_list->ResourceBarrier(1, &barrier);
            resource.state = state;
        }

        void TransitionRaw(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            g_list->ResourceBarrier(1, &barrier);
        }

        constexpr D3D12_RESOURCE_STATES STATE_SHADER_RESOURCE =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Shaders -------------------------------------------------------------------------------

        ComPtr<ID3DBlob> CompileHlsl(const std::string& source, const char* target, const char* name)
        {
            uint64_t hash = HashBytes(source.data(), source.size(), HashBytes(target, strlen(target)));
            char fileName[64];
            snprintf(fileName, sizeof(fileName), "%016llx.%s.dxbc", (unsigned long long)hash, target);
            std::filesystem::path cachePath = g_shaderCacheDir / fileName;

            std::error_code error;
            if (!g_shaderCacheDir.empty() && std::filesystem::exists(cachePath, error))
            {
                std::ifstream file(cachePath, std::ios::binary);
                std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                ComPtr<ID3DBlob> blob;
                if (!data.empty() && g_createBlob && SUCCEEDED(g_createBlob(data.size(), &blob)))
                {
                    memcpy(blob->GetBufferPointer(), data.data(), data.size());
                    return blob;
                }
            }

            if (!g_compile)
                return nullptr;
            ComPtr<ID3DBlob> blob, errors;
            HRESULT result = g_compile(source.data(), source.size(), name, nullptr, nullptr, "main", target,
                                       D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors);
            g_stats.shaderCompiles++;
            if (FAILED(result))
            {
                g_stats.shaderFailures++;
                std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "?";
                if (message.size() > 1500)
                    message.resize(1500);
                Log("[gpu] shader %s failed to compile (%s):\n%s", name, target, message.c_str());
                if (!g_shaderCacheDir.empty())
                {
                    std::filesystem::path dumpPath = g_shaderCacheDir.parent_path() / "shaderdump";
                    std::filesystem::create_directories(dumpPath, error);
                    std::ofstream(dumpPath / (std::string(name) + ".failed.hlsl")) << source;
                }
                return nullptr;
            }
            if (!g_shaderCacheDir.empty())
            {
                std::ofstream file(cachePath, std::ios::binary);
                file.write(static_cast<const char*>(blob->GetBufferPointer()), std::streamsize(blob->GetBufferSize()));
            }
            return blob;
        }

        ShaderEntry* PrepareShader(ShaderEntry* entry)
        {
            if (!entry || entry->failed)
                return nullptr;
            if (!entry->translated)
            {
                entry->translated = true;
                entry->info = TranslateShader(entry->info.stage, entry->ucode.data(), uint32_t(entry->ucode.size()));
                char name[64];
                snprintf(name, sizeof(name), "%s_%016llx", entry->info.stage == ShaderStage::Pixel ? "ps" : "vs", (unsigned long long)entry->info.hash);
                if (const char* patch = graphics::PatchShader(entry->info.stage, entry->info.hash, entry->info.hlsl))
                    Log("[gpu] %s: graphics option \"%s\" applied", name, patch);
                if (g_dumpShaders && !g_shaderCacheDir.empty())
                {
                    std::error_code error;
                    std::filesystem::path dumpPath = g_shaderCacheDir.parent_path() / "shaderdump";
                    std::filesystem::create_directories(dumpPath, error);
                    std::ofstream(dumpPath / (std::string(name) + ".hlsl")) << entry->info.hlsl;
                }
                // userdata\shaderoverride\<name>.hlsl replaces the translated HLSL (debugging and
                // shader mods; same interface as the files in shaderdump\).
                if (!g_shaderCacheDir.empty())
                {
                    std::filesystem::path overridePath = g_shaderCacheDir.parent_path() / "shaderoverride" / (std::string(name) + ".hlsl");
                    std::ifstream file(overridePath);
                    if (file)
                    {
                        entry->info.hlsl.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                        Log("[gpu] %s: using %s", name, overridePath.string().c_str());
                    }
                }
                entry->bytecode = CompileHlsl(entry->info.hlsl, entry->info.stage == ShaderStage::Pixel ? "ps_5_1" : "vs_5_1", name);
                if (!entry->bytecode)
                    entry->failed = true;
            }
            return entry->failed ? nullptr : entry;
        }

        const char* RECT_LIST_GS = R"(
struct XeVertexOutput
{
    float4 interpolators[16] : TEXCOORD0;
    float4 position : SV_Position;
};

XeVertexOutput Combine(XeVertexOutput a, XeVertexOutput b, XeVertexOutput c)
{
    XeVertexOutput d;
    [unroll] for (uint i = 0; i < 16u; i++) d.interpolators[i] = a.interpolators[i] + b.interpolators[i] - c.interpolators[i];
    d.position = a.position + b.position - c.position;
    return d;
}

// Rectangle list: 3 vertices per rectangle; the 4th corner is opposite the right angle.
[maxvertexcount(4)]
void main(triangle XeVertexOutput input[3], inout TriangleStream<XeVertexOutput> stream)
{
    float2 p0 = input[0].position.xy / input[0].position.w;
    float2 p1 = input[1].position.xy / input[1].position.w;
    float2 p2 = input[2].position.xy / input[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    uint c = (d12 >= d01 && d12 >= d20) ? 0u : ((d20 >= d01) ? 1u : 2u);
    uint a = (c + 1u) % 3u, b = (c + 2u) % 3u;
    stream.Append(input[c]);
    stream.Append(input[a]);
    stream.Append(input[b]);
    stream.Append(Combine(input[a], input[b], input[c]));
}
)";

        const char* BLIT_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

        // Resolve: copies source texels to the destination, offset by (dx, dy).
        const char* RESOLVE_PS = R"(
Texture2D<float4> source : register(t0);
cbuffer constants : register(b0)
{
    int2 offset;     // destination - source
    float scale;     // 2^exp_bias
    uint flags;      // 1: swap red/blue, 4: 4x MSAA source (two host texels per pixel along x), 8/16: two/half host rows per pixel
};
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy) - offset;
    if (flags & 4u) p.x *= 2;
    if (flags & 8u) p.y *= 2;
    if (flags & 16u) p.y /= 2;
    float4 v = source.Load(int3(p, 0));
    if (flags & 1u) v = v.bgra;
    return v * scale;
}
)";

        // Depth resolve, packed: the word the console writes for D24S8/D24FS8, (depth24 << 8) |
        // stencil, as RGBA8 bytes (R = bits 0-7). Depth is re-encoded as 20e4 float (as Xenia's
        // PreClampedFloat32To20e4) or 24-bit unorm, like the EDRAM holds it.
        const char* RESOLVE_DEPTH_PACKED_PS = R"(
Texture2D<float> depth : register(t0);
Texture2D<uint2> stencil : register(t1);
cbuffer constants : register(b0)
{
    int2 offset;     // destination - source
    float scale;     // unused
    uint flags;      // 2: depth is 20e4 float, 4: 4x MSAA source (two host texels per pixel along x), 8/16: two/half host rows per pixel
};
uint EncodeFloat24(float d)
{
    uint f = asuint(clamp(d, 0.0, 1.99999988));
    if (f < 0x38800000u)
    {
        uint shift = min(113u - (f >> 23), 24u);
        f = (0x800000u | (f & 0x7FFFFFu)) >> shift;
    }
    else
    {
        f += 0xC8000000u;
    }
    return ((f + 3u + ((f >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy) - offset;
    if (flags & 4u) p.x *= 2;
    if (flags & 8u) p.y *= 2;
    if (flags & 16u) p.y /= 2;
    float d = depth.Load(int3(p, 0));
    uint d24 = (flags & 2u) ? EncodeFloat24(d) : uint(round(saturate(d) * 16777215.0));
    uint s = stencil.Load(int3(p, 0)).y & 0xFFu;
    return float4(s, d24 & 0xFFu, (d24 >> 8) & 0xFFu, d24 >> 16) / 255.0;
}
)";

        // Depth-alias clears (see DrawImpl): the color bytes of the EDRAM word a depth-only draw
        // writes, (depth24 << 8) | stencil, with the stencil value and the depth format in
        // xe_flags.w (bits 0-7; bit 8: 20e4 float depth).
        const char* DEPTH_ALIAS_PS = R"(
cbuffer xe_system : register(b0)
{
    float4 xe_ndc_scale;
    float4 xe_ndc_offset;
    uint4 xe_flags;
};
struct XeVertexOutput
{
    float4 interpolators[16] : TEXCOORD0;
    float4 position : SV_Position;
};
uint EncodeFloat24(float d)
{
    uint f = asuint(clamp(d, 0.0, 1.99999988));
    if (f < 0x38800000u)
    {
        uint shift = min(113u - (f >> 23), 24u);
        f = (0x800000u | (f & 0x7FFFFFu)) >> shift;
    }
    else
    {
        f += 0xC8000000u;
    }
    return ((f + 3u + ((f >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}
float4 main(XeVertexOutput input) : SV_Target0
{
    float d = input.position.z;
    uint d24 = (xe_flags.w & 0x100u) ? EncodeFloat24(d) : uint(round(saturate(d) * 16777215.0));
    uint s = xe_flags.w & 0xFFu;
    return float4(s, d24 & 0xFFu, (d24 >> 8) & 0xFFu, d24 >> 16) / 255.0;
}
)";
        constexpr uint64_t DEPTH_ALIAS_PS_KEY = 0xDEA7A11A5C1EA700ull;

        const char* PRESENT_PS = R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer constants : register(b0)
{
    float2 uvScale;
    float2 targetSize;
    uint flags;      // 1: swap red/blue, 2: no source (show a pattern), 4/8: raw debug views
    uint swizzle;    // front buffer fetch constant swizzle (3 bits per component)
    float2 origin;   // top-left of the image in the target (black bars before it keep the aspect ratio)
};
float select(float4 v, uint s)
{
    if (s >= 4u) return s == 5u ? 1.0 : 0.0;
    return dot(v, float4(s == 0u, s == 1u, s == 2u, s == 3u));
}
float4 main(float4 position : SV_Position) : SV_Target
{
    if (flags & 2u)
    {
        return float4(0.08, 0.08, 0.10, 1.0);
    }
    float2 uv = (position.xy - origin) / targetSize * uvScale;
    float4 t = source.SampleLevel(linearClamp, uv, 0.0);
    if (flags & 8u)
        return float4(t.rgb, 1.0); // CRASH_GPU_SHOW_RESOLVE on a color resolve: raw texels
    if (flags & 4u)
    {
        // CRASH_GPU_SHOW_RESOLVE: raw view, x64 on red (a packed depth resolve: stencil in red,
        // depth's high byte in green).
        return float4(saturate(t.r * 64.0), t.a, t.b, 1.0);
    }
    float3 v = float3(select(t, swizzle & 7u), select(t, (swizzle >> 3) & 7u), select(t, (swizzle >> 6) & 7u));
    if (flags & 1u) v = v.bgr;
    return float4(v, 1.0);
}
)";

        // FXAA-style post-process anti-aliasing (the classic "FXAA PC console" scheme, written for
        // this project): luma edge test on the 2x2 diagonal neighbourhood, then a blend of 2 or 4
        // taps along the edge direction, keeping the 2-tap result when the 4-tap one leaves the
        // local luma range. Flat areas (contrast under the threshold) are left untouched.
        const char* FXAA_PS = R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer constants : register(b0)
{
    float2 unused;
    float2 targetSize;
};
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 Tap(float2 uv) { return source.SampleLevel(linearClamp, uv, 0.0).rgb; }
float4 main(float4 position : SV_Position) : SV_Target
{
    const float SPAN_MAX = 8.0, REDUCE_MUL = 1.0 / 8.0, REDUCE_MIN = 1.0 / 128.0;
    float2 texel = 1.0 / targetSize;
    float2 uv = position.xy * texel;
    float3 rgbM = Tap(uv);
    float lumaNW = Luma(Tap(uv + float2(-0.5, -0.5) * texel * 2.0));
    float lumaNE = Luma(Tap(uv + float2( 0.5, -0.5) * texel * 2.0));
    float lumaSW = Luma(Tap(uv + float2(-0.5,  0.5) * texel * 2.0));
    float lumaSE = Luma(Tap(uv + float2( 0.5,  0.5) * texel * 2.0));
    float lumaM = Luma(rgbM);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125))
        return float4(rgbM, 1.0);

    float2 dir = float2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * REDUCE_MUL), REDUCE_MIN);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, -SPAN_MAX, SPAN_MAX) * texel;

    float3 rgbA = 0.5 * (Tap(uv + dir * (1.0 / 3.0 - 0.5)) + Tap(uv + dir * (2.0 / 3.0 - 0.5)));
    float3 rgbB = rgbA * 0.5 + 0.25 * (Tap(uv - dir * 0.5) + Tap(uv + dir * 0.5));
    float lumaB = Luma(rgbB);
    return float4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
)";

        // Render targets ------------------------------------------------------------------------

        DXGI_FORMAT ColorRtvFormat(uint32_t format)
        {
            switch (xenos::ColorRenderTargetFormat(format))
            {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
            default: return DXGI_FORMAT_R16G16B16A16_FLOAT; // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT)
            }
        }

        DXGI_FORMAT ColorResourceFormat(DXGI_FORMAT rtv)
        {
            if (rtv == DXGI_FORMAT_R8G8B8A8_UNORM || rtv == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                return DXGI_FORMAT_R8G8B8A8_TYPELESS;
            return rtv;
        }

        DXGI_FORMAT ColorSrvFormat(DXGI_FORMAT rtv)
        {
            return rtv == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : rtv;
        }

        // Host pixels per guest pixel along x. EDRAM rows are laid out in samples: 2x MSAA puts
        // the two samples of a pixel above each other and 4x in a 2x2 square, so a 4x surface of
        // pitch P covers the same EDRAM as a 2x (or 1x) surface of pitch 2P. Host render targets
        // are keyed by row width in samples (with 1x and 2x sharing the pixel grid, as before), so
        // the 4x views D3D uses for fast clears land in the 2x target, two host pixels per pixel.
        uint32_t SamplesX(uint32_t msaa)
        {
            return msaa >= 2 ? 2u : 1u;
        }

        // Host rows per guest pixel row when a surface with `msaa` is drawn into `target`.
        // A 2x or 4x pixel spans 2 sample rows, a 1x pixel 1. So a 4x view of a target first
        // used at 1x (D3D's fast depth clear of a 1280x720 1x surface through a 640x360 4x one)
        // covers 2 host rows per pixel; a 4x view of a 2x target covers 1, as before.
        float HostRowScale(const RenderTarget& target, uint32_t msaa)
        {
            return float(msaa ? 2u : 1u) / float(target.rowSamples);
        }

        // Resolve shader flags for that scale (8: two host rows per pixel, 16: half a row).
        uint32_t RowScaleFlags(float rowScale)
        {
            return rowScale > 1.5f ? 8u : rowScale < 0.75f ? 16u : 0u;
        }

        uint64_t RenderTargetKey(bool isDepth, uint32_t base, uint32_t hostWidth, uint32_t format)
        {
            return uint64_t(base & 0xFFF) | (uint64_t(hostWidth & 0x7FFF) << 12) | (uint64_t(format & 0xF) << 27) | (uint64_t(isDepth) << 31);
        }

        // The resource and views of `target` at its scale (g_sceneScale for scene targets,
        // g_renderScale for the others), cleared. Its descriptors are allocated the first time and
        // reused when the resource is recreated at another scale (Reconfigure).
        void CreateRenderTargetResource(RenderTarget& target)
        {
            bool isDepth = target.isDepth;
            uint32_t format = target.guestFormat;
            target.texture = {};
            target.scale = target.IsScene() ? g_sceneScale : g_renderScale;
            target.hostWidth = ScaledSize(target.width, target.scale);
            target.hostHeight = ScaledSize(target.height, target.scale);

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = target.hostWidth;
            desc.Height = target.hostHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_CLEAR_VALUE clear{};

            if (isDepth)
            {
                bool isFloat = format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
                desc.Format = isFloat ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
                target.rtvFormat = isFloat ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
                target.srvFormat = isFloat ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                clear.Format = target.rtvFormat;
                clear.DepthStencil.Depth = 1.0f;
                target.texture.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            else
            {
                target.rtvFormat = ColorRtvFormat(format);
                target.srvFormat = ColorSrvFormat(target.rtvFormat);
                desc.Format = ColorResourceFormat(target.rtvFormat);
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                clear.Format = target.rtvFormat;
                target.texture.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, target.texture.state, &clear,
                                                         IID_PPV_ARGS(&target.texture.resource))))
                Fatal("[gpu] could not create a %ux%u render target", target.hostWidth, target.hostHeight);

            if (isDepth)
            {
                if (!target.view.ptr)
                    target.view = AllocateDsv();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                dsv.Format = target.rtvFormat;
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                g_device->CreateDepthStencilView(target.texture.resource.Get(), &dsv, target.view);
                g_list->ClearDepthStencilView(target.view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            }
            else
            {
                if (!target.view.ptr)
                    target.view = AllocateRtv();
                D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                rtv.Format = target.rtvFormat;
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                g_device->CreateRenderTargetView(target.texture.resource.Get(), &rtv, target.view);
                const float zero[4] = {};
                g_list->ClearRenderTargetView(target.view, zero, 0, nullptr);
            }

            if (!target.srvIndex)
                target.srvIndex = AllocateCpuSrv();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = target.srvFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(target.texture.resource.Get(), &srv, CpuSrv(target.srvIndex));
            if (isDepth)
            {
                bool isFloat = format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
                if (!target.stencilSrvIndex)
                    target.stencilSrvIndex = AllocateCpuSrv();
                srv.Format = isFloat ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                srv.Texture2D.PlaneSlice = 1;
                g_device->CreateShaderResourceView(target.texture.resource.Get(), &srv, CpuSrv(target.stencilSrvIndex));
            }
        }

        RenderTarget& GetRenderTarget(bool isDepth, uint32_t base, uint32_t pitch, uint32_t format, uint32_t msaa)
        {
            uint32_t hostWidth = std::max(pitch * SamplesX(msaa), 1u);
            auto [it, inserted] = g_renderTargets.try_emplace(RenderTargetKey(isDepth, base, hostWidth, format));
            RenderTarget& target = it->second;
            if (!inserted)
                return target;

            target.isDepth = isDepth;
            target.rowSamples = msaa ? 2u : 1u;
            target.guestFormat = format;
            target.width = hostWidth;
            target.height = RENDER_TARGET_HEIGHT;
            CreateRenderTargetResource(target);
            Log("[gpu] new %s render target: EDRAM base %u, pitch %u, MSAA %ux, format %u (%u host pixels wide)", isDepth ? "depth" : "color",
                base & 0xFFF, pitch, 1u << msaa, format, hostWidth);
            return target;
        }

        // The k_8_8_8_8 color render target over the same EDRAM as a depth surface, if one exists
        // (see the depth-alias clears in DrawImpl).
        RenderTarget* FindAliasedColorTarget(uint32_t base, uint32_t hostWidth)
        {
            auto it = g_renderTargets.find(RenderTargetKey(false, base, hostWidth, uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8)));
            return it != g_renderTargets.end() ? &it->second : nullptr;
        }

        // Textures ------------------------------------------------------------------------------

        DXGI_FORMAT ResolveDestFormat(uint32_t format, uint32_t& bytesPerPixel)
        {
            using xenos::TextureFormat;
            bytesPerPixel = 4;
            switch (TextureFormat(format))
            {
            case TextureFormat::k_8: case TextureFormat::k_8_A: case TextureFormat::k_8_B:
                bytesPerPixel = 1;
                return DXGI_FORMAT_R8_UNORM;
            case TextureFormat::k_8_8:
                bytesPerPixel = 2;
                return DXGI_FORMAT_R8G8_UNORM;
            case TextureFormat::k_5_6_5: case TextureFormat::k_1_5_5_5: case TextureFormat::k_4_4_4_4: case TextureFormat::k_6_5_5:
                bytesPerPixel = 2;
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::k_2_10_10_10: case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
                return DXGI_FORMAT_R10G10B10A2_UNORM;
            case TextureFormat::k_10_11_11: case TextureFormat::k_11_11_10:
            case TextureFormat::k_10_11_11_AS_16_16_16_16: case TextureFormat::k_11_11_10_AS_16_16_16_16:
                return DXGI_FORMAT_R11G11B10_FLOAT;
            case TextureFormat::k_16:
                bytesPerPixel = 2;
                return DXGI_FORMAT_R16_UNORM;
            case TextureFormat::k_16_16:
                return DXGI_FORMAT_R16G16_UNORM;
            case TextureFormat::k_16_16_16_16:
                bytesPerPixel = 8;
                return DXGI_FORMAT_R16G16B16A16_UNORM;
            case TextureFormat::k_16_FLOAT:
                bytesPerPixel = 2;
                return DXGI_FORMAT_R16_FLOAT;
            case TextureFormat::k_16_16_FLOAT:
                return DXGI_FORMAT_R16G16_FLOAT;
            case TextureFormat::k_16_16_16_16_FLOAT:
                bytesPerPixel = 8;
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case TextureFormat::k_32_FLOAT:
                return DXGI_FORMAT_R32_FLOAT;
            case TextureFormat::k_32_32_FLOAT:
                bytesPerPixel = 8;
                return DXGI_FORMAT_R32G32_FLOAT;
            case TextureFormat::k_32_32_32_32_FLOAT:
                bytesPerPixel = 16;
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            default:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            }
        }

        ResolveTarget* FindResolveTarget(uint32_t physicalBase)
        {
            auto it = g_resolveTargets.find(physicalBase & 0x1FFFFFFF);
            return it == g_resolveTargets.end() ? nullptr : &it->second;
        }

        ResolveTarget& CreateResolveTarget(uint32_t physicalBase, DXGI_FORMAT format, uint32_t bytesPerPixel, uint32_t width, uint32_t height, uint32_t guestFormat)
        {
            ResolveTarget& target = g_resolveTargets[physicalBase];
            if (target.format == format && target.width == width && target.height == height)
                return target;

            target = ResolveTarget{};
            target.format = format;
            target.width = width;
            target.height = height;
            target.bytesPerPixel = bytesPerPixel;
            target.guestFormat = guestFormat;
            Log("[gpu] new resolve target at 0x%08X: %ux%u, format %u (DXGI %u)", physicalBase, width, height, guestFormat, uint32_t(format));
            return target;
        }

        uint32_t SurfaceIndex(float scale)
        {
            return scale != g_renderScale ? 1u : 0u;
        }

        // The surface of `target` at `scale`, created on first use.
        ResolveSurface& EnsureSurface(ResolveTarget& target, float scale)
        {
            ResolveSurface& surface = target.surfaces[SurfaceIndex(scale)];
            if (surface.texture.resource)
                return surface;
            surface.scale = scale;
            surface.hostWidth = ScaledSize(target.width, scale);
            surface.hostHeight = ScaledSize(target.height, scale);
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = surface.hostWidth;
            desc.Height = surface.hostHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = target.format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_CLEAR_VALUE clear{};
            clear.Format = target.format;
            surface.texture.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, surface.texture.state, &clear,
                                                         IID_PPV_ARGS(&surface.texture.resource))))
                Fatal("[gpu] could not create a %ux%u resolve target", surface.hostWidth, surface.hostHeight);
            // A surface recreated at another scale (Reconfigure) keeps its descriptors.
            if (!surface.rtv.ptr)
                surface.rtv = AllocateRtv();
            g_device->CreateRenderTargetView(surface.texture.resource.Get(), nullptr, surface.rtv);
            const float zero[4] = {};
            g_list->ClearRenderTargetView(surface.rtv, zero, 0, nullptr);
            if (!surface.srvIndex)
                surface.srvIndex = AllocateCpuSrv();
            g_device->CreateShaderResourceView(surface.texture.resource.Get(), nullptr, CpuSrv(surface.srvIndex));
            return surface;
        }

        // Creates the packed depth/stencil companion of a depth resolve surface on first use.
        void EnsurePackedDepth(ResolveSurface& target)
        {
            if (target.packed.resource)
                return;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = target.hostWidth;
            desc.Height = target.hostHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_CLEAR_VALUE clear{};
            clear.Format = desc.Format;
            target.packed.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, target.packed.state, &clear,
                                                         IID_PPV_ARGS(&target.packed.resource))))
                Fatal("[gpu] could not create a %ux%u packed depth resolve target", target.hostWidth, target.hostHeight);
            if (!target.packedRtv.ptr)
                target.packedRtv = AllocateRtv();
            g_device->CreateRenderTargetView(target.packed.resource.Get(), nullptr, target.packedRtv);
            const float zero[4] = {};
            g_list->ClearRenderTargetView(target.packedRtv, zero, 0, nullptr);
            if (!target.packedSrvIndex)
                target.packedSrvIndex = AllocateCpuSrv();
            g_device->CreateShaderResourceView(target.packed.resource.Get(), nullptr, CpuSrv(target.packedSrvIndex));
        }

        // The HD replacement of the guest texture with `key`, loaded from its pack on first use;
        // nullptr if there is none (or it can't be used).
        HdTexture* FindHdTexture(uint64_t key, bool srgb, uint32_t guestWidth, uint32_t guestHeight)
        {
            if (!hd::Has(key))
                return nullptr;
            auto [it, inserted] = g_hdTextures.try_emplace(key);
            HdTexture& hdTexture = it->second;
            if (!inserted)
                return hdTexture.failed ? nullptr : &hdTexture;
            hdTexture.failed = true;
            LARGE_INTEGER t0, t1, t2, t3, frequency;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&t0);
            hd::Image image;
            if (!hd::Load(key, srgb, image))
                return nullptr;
            QueryPerformanceCounter(&t1);
            if (image.width < guestWidth || image.height < guestHeight ||
                uint64_t(image.width) * guestHeight != uint64_t(image.height) * guestWidth)
            {
                Log("[gpu] hd texture %016llX: %ux%u is not an enlargement of the guest's %ux%u with the same aspect (%s); not used",
                    (unsigned long long)key, image.width, image.height, guestWidth, guestHeight, image.source.c_str());
                return nullptr;
            }
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = image.width;
            desc.Height = image.height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = UINT16(image.levels);
            desc.Format = image.format;
            desc.SampleDesc.Count = 1;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            hdTexture.texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, hdTexture.texture.state, nullptr,
                                                         IID_PPV_ARGS(&hdTexture.texture.resource))))
            {
                Log("[gpu] hd texture %016llX: could not create a %ux%u texture (%s)", (unsigned long long)key, image.width, image.height,
                    image.source.c_str());
                return nullptr;
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[16];
            UINT footprintRows[16];
            uint64_t totalBytes;
            g_device->GetCopyableFootprints(&desc, 0, image.levels, 0, footprints, footprintRows, nullptr, &totalBytes);
            QueryPerformanceCounter(&t2);
            uint64_t offset = AllocateUpload(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            QueryPerformanceCounter(&t3);
            if (offset == UINT64_MAX)
            {
                Log("[gpu] hd texture %016llX: %llu bytes don't fit the upload ring (%s)", (unsigned long long)key,
                    (unsigned long long)totalBytes, image.source.c_str());
                hdTexture.texture.resource.Reset();
                return nullptr;
            }
            for (uint32_t level = 0; level < image.levels; level++)
            {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = footprints[level];
                footprint.Offset += offset;
                const hd::Image::Level& source = image.layout[level];
                for (uint32_t row = 0; row < source.rows && row < footprintRows[level]; row++)
                    memcpy(g_uploadCpu + footprint.Offset + uint64_t(row) * footprint.Footprint.RowPitch,
                           image.data.data() + source.offset + size_t(row) * source.rowBytes, source.rowBytes);
                D3D12_TEXTURE_COPY_LOCATION from{};
                from.pResource = g_uploadBuffer.Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                from.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = hdTexture.texture.resource.Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = level;
                g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            hdTexture.srvIndex = AllocateCpuSrv();
            g_device->CreateShaderResourceView(hdTexture.texture.resource.Get(), nullptr, CpuSrv(hdTexture.srvIndex));
            hdTexture.width = image.width;
            hdTexture.height = image.height;
            hdTexture.failed = false;
            g_stats.hdTextures++;
            LARGE_INTEGER t4;
            QueryPerformanceCounter(&t4);
            auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) { return double(b.QuadPart - a.QuadPart) * 1000.0 / double(frequency.QuadPart); };
            Log("[gpu] hd texture %016llX: %ux%u -> %ux%u, %u mip(s), DXGI %u (%s); %.1f ms (load %.1f, create %.1f, upload wait %.1f, copy %.1f)",
                (unsigned long long)key, guestWidth, guestHeight, image.width, image.height, image.levels, uint32_t(image.format), image.source.c_str(),
                ms(t0, t4), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
            return &hdTexture;
        }

        // Creates a 1-level texture from tightly packed rows (bottom row first, as the guest's).
        bool UploadSmallTexture(HdTexture& texture, DXGI_FORMAT format, uint32_t w, uint32_t h, const uint8_t* data, uint32_t rowBytes)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            texture.texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, texture.texture.state, nullptr,
                                                         IID_PPV_ARGS(&texture.texture.resource))))
                return false;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
            UINT rows;
            uint64_t totalBytes;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, nullptr, &totalBytes);
            uint64_t offset = AllocateUpload(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            if (offset == UINT64_MAX)
            {
                texture.texture.resource.Reset();
                return false;
            }
            footprint.Offset += offset;
            for (uint32_t y = 0; y < h; y++)
                memcpy(g_uploadCpu + footprint.Offset + uint64_t(y) * footprint.Footprint.RowPitch, data + size_t(y) * rowBytes, rowBytes);
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = g_uploadBuffer.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = texture.texture.resource.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            texture.srvIndex = AllocateCpuSrv();
            g_device->CreateShaderResourceView(texture.texture.resource.Get(), nullptr, CpuSrv(texture.srvIndex));
            texture.width = w, texture.height = h;
            texture.failed = false;
            return true;
        }

        // The icon of `entry` drawn for `style` (prompt_icons.h), created on first use; nullptr when
        // the game's own icon stays.
        IconTexture* FindIconTexture(TextureEntry& entry, uint32_t style)
        {
            uint64_t id = entry.iconKey * 0x9E3779B97F4A7C15ull ^ style;
            auto [it, inserted] = g_iconTextures.try_emplace(id);
            IconTexture& icon = it->second;
            if (!inserted)
                return icon.indices.failed ? nullptr : &icon;
            icon.indices.failed = true;
            std::vector<uint8_t> indices;
            std::vector<uint32_t> palette;
            uint32_t w = entry.iconWidth, h = entry.iconHeight;
            if (!settings::prompts::DrawPalettized(entry.icon, style, int(w), int(h), entry.iconBox, indices, palette))
                return nullptr;
            // The palette in the guest's texel layout: bytes B G R A (its fetch swizzle is 0x60A).
            std::vector<uint8_t> raw(256 * 4);
            for (size_t i = 0; i < 256; i++)
            {
                uint32_t c = palette[i];
                raw[i * 4 + 0] = uint8_t(c >> 16), raw[i * 4 + 1] = uint8_t(c >> 8), raw[i * 4 + 2] = uint8_t(c), raw[i * 4 + 3] = uint8_t(c >> 24);
            }
            if (!UploadSmallTexture(icon.palette, DXGI_FORMAT_R8G8B8A8_UNORM, 256, 1, raw.data(), 256 * 4) ||
                !UploadSmallTexture(icon.indices, DXGI_FORMAT_R8_UNORM, w, h, indices.data(), w))
            {
                icon.indices.failed = true;
                return nullptr;
            }
            Log("[gpu] button icon %04X (%ux%u, texture %016llX) drawn for style %08X", uint32_t(entry.icon), w, h,
                (unsigned long long)entry.iconKey, style);
            return &icon;
        }

        // The icon style for this frame (prompt_icons.h Style, once per frame).
        uint32_t IconStyle()
        {
            static uint64_t s_frame = UINT64_MAX;
            static uint32_t s_style = 0;
            if (s_frame != g_frame)
            {
                s_frame = g_frame;
                s_style = settings::prompts::Style();
            }
            return s_style;
        }

        // Returns the CPU SRV index for the texture of fetch constant `fc`, or the null SRV.
        // `hdScale`: guest texels per host texel of an HD replacement (1 without one).
        uint32_t GetTextureSrv(const xenos::TextureFetch& fetch, float* hdScale = nullptr)
        {
            if (hdScale)
                *hdScale = 1.0f;
            if (fetch.Type() != 2)
                return g_nullTextureSrv;
            g_work.textureLookups++;

            if (ResolveTarget* resolvedTarget = FindResolveTarget(fetch.BaseAddress()))
            {
                ResolveSurface* resolved = &resolvedTarget->Current();
                if (!resolved->texture.resource)
                    return g_nullTextureSrv;
                uint32_t fetchFormat = fetch.Format();
                if (resolved->packed.resource && fetchFormat != uint32_t(xenos::TextureFormat::k_24_8) &&
                    fetchFormat != uint32_t(xenos::TextureFormat::k_24_8_FLOAT))
                {
                    Transition(resolved->packed, STATE_SHADER_RESOURCE);
                    return resolved->packedSrvIndex;
                }
                Transition(resolved->texture, STATE_SHADER_RESOURCE);
                return resolved->srvIndex;
            }

            GuestTextureFormat format;
            if (!DescribeTextureFormat(fetch, format))
            {
                static uint32_t logged;
                if (logged++ < 16)
                    Log("[gpu] unsupported texture format %u", fetch.Format());
                return g_nullTextureSrv;
            }
            TextureLayout layout = ComputeTextureLayout(fetch, format);
            if (!layout.widthBlocks || !layout.heightBlocks || layout.sizeBytes > (64u << 20) ||
                uint64_t(layout.baseAddress & 0x1FFFFFFF) + layout.sizeBytes > PHYSICAL_SIZE)
                return g_nullTextureSrv;

            // Mip levels (CRASH_GPU_NO_MIPS=1: the base level only, as before).
            static const bool noMips = getenv("CRASH_GPU_NO_MIPS") != nullptr;
            uint32_t levels = noMips ? 1u : TextureLevelCount(fetch);
            uint32_t mipAddress = levels > 1 ? fetch.MipAddress() : 0u;

            uint32_t keyData[8] = { layout.baseAddress, fetch.dword[0] & 0xFFC003FC, fetch.dword[1] & 0xFFF, fetch.dword[2],
                                    uint32_t(fetch.PackedMips()), fetch.Dimension(), mipAddress, levels };
            uint64_t key = HashBytes(keyData, sizeof(keyData));
            auto [it, inserted] = g_textures.try_emplace(key);
            TextureEntry& entry = it->second;

            uint32_t width = 0, height = 0;
            // Guest layouts of the levels, sized from the host levels. Only needed to create the
            // texture and for the once-per-frame content check, not for the other lookups of the
            // frame (~20 per texture).
            static std::vector<TextureLayout> levelLayouts;
            if (inserted || entry.checkedFrame != g_frame)
            {
                HostTextureSize(layout, format, width, height);
                levelLayouts.assign(1, layout);
                for (uint32_t level = 1; level < levels; level++)
                {
                    uint32_t levelWidth = std::max(width >> level, 1u), levelHeight = std::max(height >> level, 1u);
                    TextureLayout mip = ComputeMipLayout(fetch, format, level, (levelWidth + format.blockWidth - 1) / format.blockWidth,
                                                         (levelHeight + format.blockHeight - 1) / format.blockHeight);
                    if (uint64_t(mip.baseAddress & 0x1FFFFFFF) + mip.sizeBytes > PHYSICAL_SIZE)
                        break;
                    levelLayouts.push_back(mip);
                }
                levels = uint32_t(levelLayouts.size());
            }
            if (inserted)
            {
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = width;
                desc.Height = height;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = UINT16(levels);
                desc.Format = format.hostFormat;
                desc.SampleDesc.Count = 1;
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                entry.texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, entry.texture.state, nullptr,
                                                             IID_PPV_ARGS(&entry.texture.resource))))
                {
                    Log("[gpu] could not create a %ux%u texture (format %u)", width, height, fetch.Format());
                    g_textures.erase(it);
                    return g_nullTextureSrv;
                }
                entry.srvIndex = AllocateCpuSrv();
                g_device->CreateShaderResourceView(entry.texture.resource.Get(), nullptr, CpuSrv(entry.srvIndex));
            }

            if (entry.checkedFrame != g_frame)
            {
                entry.checkedFrame = g_frame;
                // Pages untouched since the texture was protected: same content, no hash.
                bool clean = entry.watched && entry.uploaded;
                for (const TextureLayout& level : levelLayouts)
                {
                    if (clean && !watch::Clean(level.baseAddress & (PHYSICAL_SIZE - 1), level.sizeBytes))
                        clean = false;
                }
                uint64_t hash = entry.contentHash;
                if (clean)
                {
                    g_work.textureWatched++;
                }
                else
                {
                    if (entry.watched)
                    {
                        entry.watched = false;
                        entry.watchDirtied = true;
                    }
                    hash = 0;
                    g_work.textureChecks++;
                    for (const TextureLayout& level : levelLayouts)
                    {
                        hash = HashContent(g_memory.Translate(level.baseAddress), level.sizeBytes, hash);
                        g_work.textureBytes += level.sizeBytes;
                    }
                }
                if (!entry.uploaded || hash != entry.contentHash)
                {
                    entry.stableChecks = 0;
                    entry.watchDirtied = false;
                    bool dynamic = entry.uploaded && g_frame - entry.lastUploadFrame < 60;
                    entry.contentHash = hash;
                    entry.uploaded = true;
                    entry.lastUploadFrame = g_frame;
                    entry.hd = nullptr;
                    entry.icon = 0;
                    entry.iconTexture = nullptr;

                    D3D12_RESOURCE_DESC desc = entry.texture.resource->GetDesc();
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[16];
                    UINT footprintRows[16];
                    uint64_t totalBytes;
                    g_device->GetCopyableFootprints(&desc, 0, levels, 0, footprints, footprintRows, nullptr, &totalBytes);
                    uint64_t offset = AllocateUpload(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                    if (offset == UINT64_MAX)
                        return g_nullTextureSrv;
                    Transition(entry.texture, D3D12_RESOURCE_STATE_COPY_DEST);
                    // HD packs key the decoded base level; it is decoded into host memory first,
                    // since the upload heap is write-combined (very slow to read back).
                    bool hdKeyed = (hd::Active() || hd::Dumping()) && hd::Replaceable(format.hostFormat);
                    // The game's button icons are small 8-bit index textures (prompt_icons.h).
                    bool iconKeyed = g_promptIcons && format.hostFormat == DXGI_FORMAT_R8_UNORM && width <= 128 && height <= 128;
                    g_hdKeyScratch.clear();
                    for (uint32_t level = 0; level < levels; level++)
                    {
                        const TextureLayout& levelLayout = levelLayouts[level];
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = footprints[level];
                        footprint.Offset += offset;
                        uint32_t rowBytes, rows;
                        HostTextureRows(levelLayout, format, rowBytes, rows);
                        if (rows > footprintRows[level] || rowBytes > footprint.Footprint.RowPitch)
                            continue;  // not expected: the level layouts are sized from the host levels
                        if (g_textureScratch.size() < levelLayout.sizeBytes)
                            g_textureScratch.resize(levelLayout.sizeBytes);
                        if (level == 0 && (hdKeyed || iconKeyed))
                        {
                            g_hdKeyScratch.resize(size_t(footprint.Footprint.RowPitch) * rows);
                            DecodeTexture(g_memory.Translate(levelLayout.baseAddress), g_textureScratch.data(), levelLayout, format, fetch.Endianness(),
                                          g_hdKeyScratch.data(), footprint.Footprint.RowPitch);
                            memcpy(g_uploadCpu + footprint.Offset, g_hdKeyScratch.data(), g_hdKeyScratch.size());
                        }
                        else
                            DecodeTexture(g_memory.Translate(levelLayout.baseAddress), g_textureScratch.data(), levelLayout, format, fetch.Endianness(),
                                          g_uploadCpu + footprint.Offset, footprint.Footprint.RowPitch);
                        D3D12_TEXTURE_COPY_LOCATION source{};
                        source.pResource = g_uploadBuffer.Get();
                        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        source.PlacedFootprint = footprint;
                        D3D12_TEXTURE_COPY_LOCATION destination{};
                        destination.pResource = entry.texture.resource.Get();
                        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        destination.SubresourceIndex = level;
                        g_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                    }
                    g_stats.textureUploads++;
                    g_work.textureUploads++;

                    if (iconKeyed && !g_hdKeyScratch.empty())
                    {
                        uint32_t rowBytes, rows;
                        HostTextureRows(levelLayouts[0], format, rowBytes, rows);
                        uint64_t key = hd::Key(g_hdKeyScratch.data(), footprints[0].Footprint.RowPitch, rowBytes, rows, format.hostFormat, width, height);
                        if (char32_t icon = settings::prompts::IconOfTexture(key, entry.iconBox))
                        {
                            entry.icon = icon;
                            entry.iconKey = key;
                            entry.iconWidth = width, entry.iconHeight = height;
                            entry.iconStyle = UINT32_MAX;  // picked at the next bind
                        }
                    }
                    if (hdKeyed && !g_hdKeyScratch.empty())
                    {
                        uint32_t rowBytes, rows;
                        HostTextureRows(levelLayouts[0], format, rowBytes, rows);
                        const uint8_t* base = g_hdKeyScratch.data();
                        uint32_t rowPitch = footprints[0].Footprint.RowPitch;
                        uint64_t hdKey = hd::Key(base, rowPitch, rowBytes, rows, format.hostFormat, width, height);
                        if (!dynamic)
                            hd::Dump(hdKey, base, rowPitch, format.hostFormat, width, height, fetch.Format(), fetch.Swizzle(), levels);
                        if (hd::Active())
                        {
                            entry.hd = FindHdTexture(hdKey, hd::IsSrgb(format.hostFormat), width, height);
                            entry.hdScale = entry.hd ? float(width) / float(entry.hd->width) : 1.0f;
                        }

                    }
                }
                else if (!clean)
                {
                    // Same content as last time. Written while watched without changing: a page
                    // shared with something else the game writes.
                    if (entry.watchDirtied)
                    {
                        entry.watchDirtied = false;
                        entry.watchMisses++;
                    }
                    // Stable for a few frames: protect its pages, then hash again to catch a write
                    // that came between the hash above and the protection.
                    if (++entry.stableChecks >= 3 && entry.watchMisses < 4 && watch::Enabled())
                    {
                        for (const TextureLayout& level : levelLayouts)
                            watch::Protect(level.baseAddress & (PHYSICAL_SIZE - 1), level.sizeBytes);
                        uint64_t check = 0;
                        for (const TextureLayout& level : levelLayouts)
                            check = HashContent(g_memory.Translate(level.baseAddress), level.sizeBytes, check);
                        entry.watched = check == hash;
                        if (!entry.watched)
                            entry.stableChecks = 0;
                        g_work.textureProtects++;
                    }
                }
            }
            if (entry.icon)
            {
                uint32_t style = IconStyle();
                if (style != entry.iconStyle)
                {
                    entry.iconStyle = style;
                    entry.iconTexture = style ? FindIconTexture(entry, style) : nullptr;
                }
                if (entry.iconTexture)
                {
                    Transition(entry.iconTexture->indices.texture, STATE_SHADER_RESOURCE);
                    Transition(entry.iconTexture->palette.texture, STATE_SHADER_RESOURCE);
                    g_drawIconPalette = entry.iconTexture->palette.srvIndex;
                    return entry.iconTexture->indices.srvIndex;
                }
            }
            if (entry.hd)
            {
                Transition(entry.hd->texture, STATE_SHADER_RESOURCE);
                if (hdScale)
                    *hdScale = entry.hdScale;
                return entry.hd->srvIndex;
            }
            Transition(entry.texture, STATE_SHADER_RESOURCE);
            return entry.srvIndex;
        }

        // A sampler for an HD replacement `1 / hdScale` times larger than the guest texture: the
        // guest's mip range shifted to the same texel sizes. A base-map-only sampler may still use
        // the HD levels down to the guest's size, so minification doesn't alias more than the guest.
        void AdjustSamplerForHd(D3D12_SAMPLER_DESC& desc, float hdScale)
        {
            float shift = std::log2(1.0f / hdScale);
            if (desc.MaxLOD == 0.0f)
            {
                desc.MaxLOD = shift;
                if (desc.Filter != D3D12_FILTER_ANISOTROPIC)
                    desc.Filter = D3D12_FILTER(desc.Filter | 1);  // linear between mips
            }
            else if (desc.MaxLOD < D3D12_FLOAT32_MAX)
                desc.MaxLOD += shift;
            if (desc.MinLOD > 0.0f)
                desc.MinLOD += shift;
        }

        D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp)
        {
            switch (xenos::ClampMode(clamp))
            {
            case xenos::ClampMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case xenos::ClampMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case xenos::ClampMode::MirrorClampToEdge:
            case xenos::ClampMode::MirrorClampToHalfway: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
            case xenos::ClampMode::ClampToBorder:
            case xenos::ClampMode::MirrorClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
        }

        // isTexture: a texture from guest memory (not a resolve target); the anisotropic filtering
        // option applies to those when they are filtered linearly.
        D3D12_SAMPLER_DESC MakeSampler(const xenos::TextureFetch& fetch, const TextureBinding* binding, bool isTexture)
        {
            uint32_t mag = fetch.MagFilter(), min = fetch.MinFilter(), mip = fetch.MipFilter();
            uint32_t aniso = fetch.AnisoFilter();
            if (binding)
            {
                if (binding->magFilter != 3) mag = binding->magFilter;
                if (binding->minFilter != 3) min = binding->minFilter;
                if (binding->mipFilter != 3) mip = binding->mipFilter;
                if (binding->anisoFilter != 7) aniso = binding->anisoFilter;
            }
            D3D12_SAMPLER_DESC desc{};
            bool linearMag = mag == 1, linearMin = min == 1, linearMip = mip == 1;
            uint32_t guestAnisotropy = aniso > 1 ? 1u << std::min(aniso - 1, 4u) : 1u;
            uint32_t anisotropy = guestAnisotropy;
            if (isTexture && linearMin && linearMag)
                anisotropy = std::max(anisotropy, uint32_t(g_anisotropy));
            if (anisotropy > 1)
            {
                desc.Filter = D3D12_FILTER_ANISOTROPIC;
                desc.MaxAnisotropy = anisotropy;
            }
            else
            {
                desc.Filter = D3D12_ENCODE_BASIC_FILTER(linearMin ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                                        linearMag ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                                        linearMip ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                                        D3D12_FILTER_REDUCTION_TYPE_STANDARD);
                desc.MaxAnisotropy = 1;
            }
            desc.AddressU = AddressMode(fetch.ClampX());
            desc.AddressV = AddressMode(fetch.ClampY());
            desc.AddressW = AddressMode(fetch.ClampZ());
            desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            float border = fetch.BorderColor() == 1 ? 1.0f : 0.0f;
            desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = border;
            desc.BorderColor[3] = fetch.BorderColor() == 0 ? 0.0f : 1.0f;
            // Mip filter 2 = base map: no mips. Otherwise the fetch constant's level range.
            desc.MinLOD = float(fetch.MipMinLevel());
            desc.MaxLOD = mip == 2 ? 0.0f : float(std::max(fetch.MipMaxLevel(), fetch.MipMinLevel()));
            if (mip == 2)
                desc.MinLOD = 0.0f;
            return desc;
        }

        // Pipelines -----------------------------------------------------------------------------

        D3D12_BLEND BlendFactor(uint32_t factor, bool alpha)
        {
            switch (xenos::BlendFactor(factor))
            {
            case xenos::BlendFactor::Zero: return D3D12_BLEND_ZERO;
            case xenos::BlendFactor::One: return D3D12_BLEND_ONE;
            case xenos::BlendFactor::SrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
            case xenos::BlendFactor::OneMinusSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
            case xenos::BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
            case xenos::BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
            case xenos::BlendFactor::DstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
            case xenos::BlendFactor::OneMinusDstColor: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
            case xenos::BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
            case xenos::BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
            case xenos::BlendFactor::ConstantColor:
            case xenos::BlendFactor::ConstantAlpha: return D3D12_BLEND_BLEND_FACTOR;
            case xenos::BlendFactor::OneMinusConstantColor:
            case xenos::BlendFactor::OneMinusConstantAlpha: return D3D12_BLEND_INV_BLEND_FACTOR;
            case xenos::BlendFactor::SrcAlphaSaturate: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
            default: return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND_OP BlendOp(uint32_t op)
        {
            switch (xenos::BlendOp(op))
            {
            case xenos::BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
            case xenos::BlendOp::Min: return D3D12_BLEND_OP_MIN;
            case xenos::BlendOp::Max: return D3D12_BLEND_OP_MAX;
            case xenos::BlendOp::RevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
            default: return D3D12_BLEND_OP_ADD;
            }
        }

        D3D12_COMPARISON_FUNC CompareFunc(uint32_t func)
        {
            return D3D12_COMPARISON_FUNC(D3D12_COMPARISON_FUNC_NEVER + (func & 7));
        }

        D3D12_STENCIL_OP StencilOp(uint32_t op)
        {
            constexpr D3D12_STENCIL_OP ops[8] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE,
                                                  D3D12_STENCIL_OP_INCR_SAT, D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT,
                                                  D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR };
            return ops[op & 7];
        }

        struct PipelineKey
        {
            uint64_t vs, ps;
            uint32_t rtvFormats[4];
            uint32_t dsvFormat;
            uint32_t blend[4];
            uint32_t colorMask;
            uint32_t depthControl;
            uint32_t stencilMasks;
            uint32_t cullMode;
            uint32_t frontCounterClockwise;
            int32_t depthBias;
            float slopeBias;
            uint32_t depthClip;
            uint32_t topologyType;
            uint32_t rectList;
            uint32_t stripCut;
            uint32_t renderTargetCount;
        };

        // psBlob: a renderer pixel shader instead of a guest one (key.ps must then identify it).
        ID3D12PipelineState* GetPipeline(const PipelineKey& key, ShaderEntry* vs, ShaderEntry* ps, ID3DBlob* psBlob = nullptr)
        {
            uint64_t hash = HashBytes(&key, sizeof(key));
            auto it = g_pipelines.find(hash);
            if (it != g_pipelines.end())
                return it->second.Get();

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_drawRootSignature.Get();
            desc.VS = { vs->bytecode->GetBufferPointer(), vs->bytecode->GetBufferSize() };
            if (ps)
                desc.PS = { ps->bytecode->GetBufferPointer(), ps->bytecode->GetBufferSize() };
            if (psBlob)
                desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
            if (key.rectList)
                desc.GS = { g_rectListGs->GetBufferPointer(), g_rectListGs->GetBufferSize() };

            desc.BlendState.IndependentBlendEnable = TRUE;
            for (uint32_t i = 0; i < 4; i++)
            {
                auto& rt = desc.BlendState.RenderTarget[i];
                uint32_t blend = key.blend[i];
                uint32_t colorSrc = blend & 0x1F, colorOp = (blend >> 5) & 7, colorDst = (blend >> 8) & 0x1F;
                uint32_t alphaSrc = (blend >> 16) & 0x1F, alphaOp = (blend >> 21) & 7, alphaDst = (blend >> 24) & 0x1F;
                rt.BlendEnable = !(colorSrc == 1 && colorDst == 0 && colorOp == 0 && alphaSrc == 1 && alphaDst == 0 && alphaOp == 0);
                rt.SrcBlend = BlendFactor(colorSrc, false);
                rt.DestBlend = BlendFactor(colorDst, false);
                rt.BlendOp = BlendOp(colorOp);
                rt.SrcBlendAlpha = BlendFactor(alphaSrc, true);
                rt.DestBlendAlpha = BlendFactor(alphaDst, true);
                rt.BlendOpAlpha = BlendOp(alphaOp);
                rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt.RenderTargetWriteMask = UINT8((key.colorMask >> (4 * i)) & 0xF);
            }
            desc.SampleMask = UINT_MAX;

            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE(key.cullMode);
            desc.RasterizerState.FrontCounterClockwise = key.frontCounterClockwise;
            desc.RasterizerState.DepthBias = key.depthBias;
            desc.RasterizerState.SlopeScaledDepthBias = key.slopeBias;
            desc.RasterizerState.DepthClipEnable = key.depthClip;

            uint32_t depthControl = key.depthControl;
            if (key.dsvFormat)
            {
                bool stencil = depthControl & 1;
                desc.DepthStencilState.DepthEnable = (depthControl >> 1) & 1;
                desc.DepthStencilState.DepthWriteMask = ((depthControl >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                desc.DepthStencilState.DepthFunc = CompareFunc((depthControl >> 4) & 7);
                desc.DepthStencilState.StencilEnable = stencil;
                desc.DepthStencilState.StencilReadMask = UINT8((key.stencilMasks >> 8) & 0xFF);
                desc.DepthStencilState.StencilWriteMask = UINT8((key.stencilMasks >> 16) & 0xFF);
                desc.DepthStencilState.FrontFace.StencilFunc = CompareFunc((depthControl >> 8) & 7);
                desc.DepthStencilState.FrontFace.StencilFailOp = StencilOp(depthControl >> 11);
                desc.DepthStencilState.FrontFace.StencilPassOp = StencilOp(depthControl >> 14);
                desc.DepthStencilState.FrontFace.StencilDepthFailOp = StencilOp(depthControl >> 17);
                if ((depthControl >> 7) & 1)
                {
                    desc.DepthStencilState.BackFace.StencilFunc = CompareFunc((depthControl >> 20) & 7);
                    desc.DepthStencilState.BackFace.StencilFailOp = StencilOp(depthControl >> 23);
                    desc.DepthStencilState.BackFace.StencilPassOp = StencilOp(depthControl >> 26);
                    desc.DepthStencilState.BackFace.StencilDepthFailOp = StencilOp(depthControl >> 29);
                }
                else
                {
                    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
                }
                desc.DSVFormat = DXGI_FORMAT(key.dsvFormat);
            }

            desc.IBStripCutValue = key.stripCut ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topologyType);
            desc.NumRenderTargets = key.renderTargetCount;
            for (uint32_t i = 0; i < key.renderTargetCount; i++)
                desc.RTVFormats[i] = DXGI_FORMAT(key.rtvFormats[i]);
            desc.SampleDesc.Count = 1;

            ComPtr<ID3D12PipelineState> pipeline;
            HRESULT result = g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
            if (FAILED(result))
            {
                Log("[gpu] CreateGraphicsPipelineState failed (0x%08X) for vs %016llx ps %016llx", uint32_t(result),
                    (unsigned long long)key.vs, (unsigned long long)key.ps);
                DrainDebugMessages();
            }
            g_stats.pipelines++;
            g_pipelines[hash] = pipeline;
            return pipeline.Get();
        }

        ID3D12PipelineState* GetResolvePipeline(DXGI_FORMAT format)
        {
            auto it = g_resolvePipelines.find(uint32_t(format));
            if (it != g_resolvePipelines.end())
                return it->second.Get();
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_blitRootSignature.Get();
            desc.VS = { g_blitVs->GetBufferPointer(), g_blitVs->GetBufferSize() };
            desc.PS = { g_resolvePs->GetBufferPointer(), g_resolvePs->GetBufferSize() };
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = format;
            desc.SampleDesc.Count = 1;
            ComPtr<ID3D12PipelineState> pipeline;
            if (FAILED(g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
                Log("[gpu] could not create the resolve pipeline for DXGI format %u", uint32_t(format));
            g_resolvePipelines[uint32_t(format)] = pipeline;
            return pipeline.Get();
        }

        ID3D12PipelineState* GetResolveDepthPackedPipeline()
        {
            static ComPtr<ID3D12PipelineState> pipeline;
            static bool tried;
            if (tried)
                return pipeline.Get();
            tried = true;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_blit2RootSignature.Get();
            desc.VS = { g_blitVs->GetBufferPointer(), g_blitVs->GetBufferSize() };
            desc.PS = { g_resolveDepthPackedPs->GetBufferPointer(), g_resolveDepthPackedPs->GetBufferSize() };
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            if (FAILED(g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
                Log("[gpu] could not create the packed depth resolve pipeline");
            return pipeline.Get();
        }

        void BindDescriptorHeaps()
        {
            if (!g_listNeedsHeaps)
                return;
            ID3D12DescriptorHeap* heaps[] = { g_gpuSrvHeap.Get(), g_samplerHeap.Get() };
            g_list->SetDescriptorHeaps(2, heaps);
            g_listNeedsHeaps = false;
        }

        // Scissor (window scissor + offset, intersected with the screen scissor), clamped to the
        // render target. As in Xenia's draw_util::GetScissor.
        D3D12_RECT GuestScissor(uint32_t width, uint32_t height)
        {
            uint32_t tl = Reg(xenos::REG_PA_SC_WINDOW_SCISSOR_TL);
            uint32_t br = Reg(xenos::REG_PA_SC_WINDOW_SCISSOR_BR);
            int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
            int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
            if (!((tl >> 31) & 1))
            {
                uint32_t offset = Reg(xenos::REG_PA_SC_WINDOW_OFFSET);
                int32_t ox = xenos::SignedBits(offset, 0, 15), oy = xenos::SignedBits(offset, 16, 15);
                x0 += ox; x1 += ox; y0 += oy; y1 += oy;
            }
            uint32_t stl = Reg(xenos::REG_PA_SC_SCREEN_SCISSOR_TL), sbr = Reg(xenos::REG_PA_SC_SCREEN_SCISSOR_BR);
            x0 = std::max(x0, xenos::SignedBits(stl, 0, 15));
            y0 = std::max(y0, xenos::SignedBits(stl, 16, 15));
            x1 = std::min(x1, xenos::SignedBits(sbr, 0, 15));
            y1 = std::min(y1, xenos::SignedBits(sbr, 16, 15));
            x0 = std::clamp(x0, 0, int32_t(width));
            y0 = std::clamp(y0, 0, int32_t(height));
            x1 = std::clamp(x1, x0, int32_t(width));
            y1 = std::clamp(y1, y0, int32_t(height));
            return D3D12_RECT{ x0, y0, x1, y1 };
        }

        // Resolve -------------------------------------------------------------------------------

        int32_t ToFixed16p8(float value)
        {
            return int32_t(std::lround(std::clamp(value, -32768.0f, 32767.0f) * 256.0f));
        }

        // Temporal upscaling --------------------------------------------------------------------
        //
        // With an upscaler on (upscaler.h, settings [graphics] upscaler), the scene (the MSAA
        // render targets, at g_sceneScale) is drawn with a sub-pixel jitter. Before the first 2D draw
        // that reads the scene's color resolve, the upscaler turns that resolve (render size), the
        // scene's depth resolve and the motion vectors into an output-size image, which becomes the
        // resolve target's current surface: the game's post-processing (motion blur, depth of
        // field) and HUD then draw at the output size, unjittered.
        //
        // Motion vectors: the game writes a velocity map (color 1 of the scene, the input of its
        // motion blur) only for moving objects: saturate(0.5 + (ndc now - ndc a frame ago) * K)
        // with alpha 1 (vs 429975a046a1a236, K = c255.x); everything else keeps the clear (alpha 0)
        // and gets the camera's reprojection of its depth. The view-projection matrix comes from
        // the constants of the scene's vertex shaders (c4-c7 = model-view-projection, c12-c15 =
        // model, so VP = MVP * model^-1), taking the value most draws of the frame agree on.
        const char* MOTION_PS = R"(
Texture2D<float> depthTex : register(t0);
Texture2D<float4> velocityTex : register(t1);
cbuffer constants : register(b0)
{
    row_major float4x4 inverseViewProj;   // this frame, without the jitter
    row_major float4x4 previousViewProj;  // last frame
    float2 renderSize;                    // pixels of the scene region
    float zScale;                         // window depth = ndc z * zScale + zOffset
    float zOffset;
    float velocityScale;                  // K
    uint flags;                           // 1: velocity map, 2: camera matrices
};
float2 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float2 uv = position.xy / renderSize;
    if (flags & 1u)
    {
        // Resolved with red/blue swapped: x in blue, y in green.
        float4 v = velocityTex.Load(int3(p, 0));
        if (v.a > 0.5)
        {
            float2 d = (float2(v.b, v.g) - 0.5) / velocityScale;
            return float2(-d.x, d.y) * 0.5;
        }
    }
    if (!(flags & 2u))
        return 0.0;
    float depth = depthTex.Load(int3(p, 0));
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, (depth - zOffset) / zScale, 1.0);
    float4 world = mul(inverseViewProj, clip);
    world /= world.w;
    float4 previous = mul(previousViewProj, world);
    float2 previousNdc = previous.xy / previous.w;
    return float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5) - uv;
}
)";
        // CRASH_GPU_SHOW_MOTION=<scale>: the scene shows its motion vectors instead of the
        // upscaler's output (red/green = 0.5 + x/y * scale; blue = 1 where the game's velocity map
        // wrote the vector).
        const char* MOTION_VIEW_PS = R"(
Texture2D<float2> motion : register(t0);
Texture2D<float4> velocityTex : register(t1);
cbuffer constants : register(b0) { float scale; float3 unused; };
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float2 m = motion.Load(int3(p, 0));
    return float4(0.5 + m * scale, velocityTex.Load(int3(p, 0)).a > 0.5 ? 1.0 : 0.0, 1.0);
}
)";

        constexpr uint64_t SCENE_VS_GENERIC = 0xAECAB5202246C470ull;  // c4-c7 MVP, c12-c15 model
        constexpr uint64_t SCENE_VS_VELOCITY = 0x429975A046A1A236ull; // same, + c16-c19 previous MVP, c255.x = K

        struct Temporal
        {
            bool active = false;            // an upscaler runs (upscaler::Active)
            // This frame's scene resolves.
            ResolveTarget* color = nullptr;
            ResolveTarget* depth = nullptr;
            ResolveTarget* velocity = nullptr;
            bool done = false;              // evaluated this frame
            uint64_t lastEvaluated = UINT64_MAX;
            // Jitter of this frame: render pixels and guest pixels.
            float jitterX = 0.0f, jitterY = 0.0f;
            float jitterGuestX = 0.0f, jitterGuestY = 0.0f;
            // Camera: candidate view-projection matrices of this frame's scene draws.
            std::vector<std::array<double, 16>> candidates;
            std::array<double, 16> viewProj{}, previousViewProj{};
            bool haveViewProj = false, havePrevious = false;
            float zScale = 1.0f, zOffset = 0.0f;
            float velocityScale = 5.0f;
            // Resources.
            ComPtr<ID3D12RootSignature> rootSignature;
            ComPtr<ID3D12PipelineState> motionPipeline, motionViewPipeline;
            float showMotion = 0.0f;  // CRASH_GPU_SHOW_MOTION
            TrackedResource motion, output;
            D3D12_CPU_DESCRIPTOR_HANDLE motionRtv{};
            uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
            LARGE_INTEGER lastTime{};
            uint64_t evaluations = 0, failures = 0;
            double evaluateMs = 0.0;  // CPU time in upscaler::Evaluate since the last stats line
        } g_temporal;

        // Lighting remaster (lighting.h): this frame's options (from the settings, every Swap). It
        // runs where the upscaler does, so the scene's resolves and camera are noted for it too.
        lighting::Options g_lightingOptions;

        bool ScenePassActive()
        {
            return g_temporal.active || g_lightingOptions.Any();
        }

        // Column-major products: m[col * 4 + row]. a * b.
        std::array<double, 16> Multiply(const std::array<double, 16>& a, const std::array<double, 16>& b)
        {
            std::array<double, 16> r{};
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++)
                {
                    double sum = 0.0;
                    for (int k = 0; k < 4; k++)
                        sum += a[k * 4 + row] * b[c * 4 + k];
                    r[c * 4 + row] = sum;
                }
            return r;
        }

        bool Invert(const std::array<double, 16>& m, std::array<double, 16>& out)
        {
            // Gauss-Jordan on a row-major copy.
            double a[4][8];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    a[r][c] = m[c * 4 + r];
                    a[r][c + 4] = r == c ? 1.0 : 0.0;
                }
            for (int c = 0; c < 4; c++)
            {
                int pivot = c;
                for (int r = c + 1; r < 4; r++)
                    if (fabs(a[r][c]) > fabs(a[pivot][c]))
                        pivot = r;
                if (fabs(a[pivot][c]) < 1e-12)
                    return false;
                for (int k = 0; k < 8; k++)
                    std::swap(a[c][k], a[pivot][k]);
                double inv = 1.0 / a[c][c];
                for (int k = 0; k < 8; k++)
                    a[c][k] *= inv;
                for (int r = 0; r < 4; r++)
                {
                    if (r == c)
                        continue;
                    double f = a[r][c];
                    for (int k = 0; k < 8; k++)
                        a[r][k] -= f * a[c][k];
                }
            }
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    out[c * 4 + r] = a[r][c + 4];
            return true;
        }

        // A scene draw's vertex shader constants (the big-endian register words, c0 first): the
        // camera.
        void NoteSceneCamera(uint64_t vsHash, const uint32_t* words, float zScale, float zOffset)
        {
            if (vsHash != SCENE_VS_GENERIC && vsHash != SCENE_VS_VELOCITY)
                return;
            float constants[256 * 4];
            for (uint32_t c : { 4u, 5u, 6u, 7u, 12u, 13u, 14u, 15u, 255u })
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t bits = __builtin_bswap32(words[c * 4 + i]);
                    memcpy(&constants[c * 4 + i], &bits, 4);
                }
            if (vsHash == SCENE_VS_VELOCITY && constants[255 * 4] > 0.0f)
                g_temporal.velocityScale = constants[255 * 4];
            if (g_temporal.candidates.size() >= 64)
                return;
            std::array<double, 16> mvp, model, inverseModel;
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                {
                    mvp[c * 4 + r] = constants[(4 + c) * 4 + r];
                    model[c * 4 + r] = constants[(12 + c) * 4 + r];
                }
            if (!Invert(model, inverseModel))
                return;
            g_temporal.candidates.push_back(Multiply(mvp, inverseModel));
            g_temporal.zScale = zScale;
            g_temporal.zOffset = zOffset;
        }

        // The view-projection most candidates agree on (the sky or a few odd draws may differ).
        bool ChooseViewProj(std::array<double, 16>& out)
        {
            const auto& list = g_temporal.candidates;
            size_t best = SIZE_MAX, bestVotes = 0;
            for (size_t i = 0; i < list.size(); i++)
            {
                double norm = 0.0;
                for (double v : list[i])
                    norm = std::max(norm, fabs(v));
                size_t votes = 0;
                for (size_t j = 0; j < list.size(); j++)
                {
                    double difference = 0.0;
                    for (int k = 0; k < 16; k++)
                        difference = std::max(difference, fabs(list[i][k] - list[j][k]));
                    votes += difference <= 1e-3 * std::max(norm, 1e-6);
                }
                if (votes > bestVotes)
                    best = i, bestVotes = votes;
            }
            if (best == SIZE_MAX)
                return false;
            out = list[best];
            return true;
        }

        // Near, far and vertical field of view of a D3D-style projection inside VP = P * V
        // (V rigid): VP's row 3 is V's z row, row 1 = sy * V's y row, row 2 = a * row 3 + b.
        void CameraParameters(const std::array<double, 16>& vp, float& nearPlane, float& farPlane, float& fovY)
        {
            auto row = [&](int r, int c) { return vp[c * 4 + r]; };
            double r3[3] = { row(3, 0), row(3, 1), row(3, 2) }, r1[3] = { row(1, 0), row(1, 1), row(1, 2) }, r2[3] = { row(2, 0), row(2, 1), row(2, 2) };
            double len3 = sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
            double sy = sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]) / std::max(len3, 1e-9);
            double a = (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / std::max(len3 * len3, 1e-12);
            double b = (row(2, 3) - a * row(3, 3)) / std::max(len3, 1e-9);
            fovY = float(2.0 * atan(1.0 / std::max(sy, 1e-6)));
            double n = fabs(a) > 1e-9 ? -b / a : 0.1, f = fabs(1.0 - a) > 1e-9 ? b / (1.0 - a) : 1000.0;
            n = fabs(n), f = fabs(f);
            if (n > f)
                std::swap(n, f);
            nearPlane = float(std::clamp(n, 1e-3, 1e5));
            farPlane = float(std::clamp(f, double(nearPlane) * 2.0, 1e7));
            if (!(fovY > 0.05f && fovY < 3.0f))
                fovY = 1.0f;
        }

        // The upscaler's inputs: the scene's color, depth and velocity resolves of this frame.
        void NoteSceneResolve(ResolveTarget& destination, const RenderTarget& source, bool isDepth, uint32_t sourceSelect)
        {
            if (!ScenePassActive() || !source.IsScene())
                return;
            if (isDepth)
                g_temporal.depth = &destination;
            else if (sourceSelect == 0)
                g_temporal.color = &destination;
            else if (sourceSelect == 1)
                g_temporal.velocity = &destination;
        }

        bool EnsureTemporalTargets(uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight)
        {
            Temporal& t = g_temporal;
            if (t.motion.resource && t.renderWidth == renderWidth && t.renderHeight == renderHeight && t.outputWidth == outputWidth &&
                t.outputHeight == outputHeight)
                return true;
            if (t.motion.resource)
                WaitIdle();
            t.motion = {};
            t.output = {};
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Width = renderWidth;
            desc.Height = renderHeight;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = desc.Format;
            t.motion.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t.motion.state, &clear, IID_PPV_ARGS(&t.motion.resource))))
                return false;
            if (!t.motionRtv.ptr)
                t.motionRtv = AllocateRtv();
            g_device->CreateRenderTargetView(t.motion.resource.Get(), nullptr, t.motionRtv);
            desc.Width = outputWidth;
            desc.Height = outputHeight;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            t.output.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t.output.state, nullptr, IID_PPV_ARGS(&t.output.resource))))
            {
                t.motion = {};
                return false;
            }
            t.renderWidth = renderWidth;
            t.renderHeight = renderHeight;
            t.outputWidth = outputWidth;
            t.outputHeight = outputHeight;
            Log("[gpu] temporal upscaling (%s): scene %ux%u -> %ux%u", upscaler::ActiveName().c_str(), renderWidth, renderHeight, outputWidth, outputHeight);
            return true;
        }

        bool CreateTemporalPipeline()
        {
            D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor = { 0, 0 };
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1].DescriptorTable = { 1, &range };
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC desc{ 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
                FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_temporal.rootSignature))))
                return false;
            ComPtr<ID3DBlob> ps = CompileHlsl(MOTION_PS, "ps_5_1", "motion_vectors_ps");
            if (!ps)
                return false;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_temporal.rootSignature.Get();
            pso.VS = { g_blitVs->GetBufferPointer(), g_blitVs->GetBufferSize() };
            pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
            pso.SampleDesc.Count = 1;
            if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_temporal.motionPipeline))))
                return false;
            if (const char* show = getenv("CRASH_GPU_SHOW_MOTION"))
            {
                g_temporal.showMotion = float(atof(show));
                ComPtr<ID3DBlob> view = CompileHlsl(MOTION_VIEW_PS, "ps_5_1", "motion_view_ps");
                if (view)
                {
                    pso.PS = { view->GetBufferPointer(), view->GetBufferSize() };
                    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                }
                if (!view || FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_temporal.motionViewPipeline))))
                    g_temporal.showMotion = 0.0f;
            }
            return true;
        }

        // Runs the upscaler on this frame's scene (see "Temporal upscaling" above).
        void RunTemporal()
        {
            InvalidateDrawState();
            Temporal& t = g_temporal;
            t.done = true;
            ResolveTarget& color = *t.color;
            ResolveSurface& input = color.surfaces[SurfaceIndex(g_sceneScale)];
            ResolveSurface* depth = t.depth ? &t.depth->surfaces[SurfaceIndex(g_sceneScale)] : nullptr;
            ResolveSurface* velocity = t.velocity ? &t.velocity->surfaces[SurfaceIndex(g_sceneScale)] : nullptr;
            if (!input.texture.resource || !depth || !depth->texture.resource || t.depth->format != DXGI_FORMAT_R32_FLOAT)
                return;
            uint32_t renderWidth = input.hostWidth, renderHeight = input.hostHeight;
            uint32_t outputWidth = ScaledSize(color.width, g_renderScale), outputHeight = ScaledSize(color.height, g_renderScale);
            if (depth->hostWidth < renderWidth || depth->hostHeight < renderHeight || !EnsureTemporalTargets(renderWidth, renderHeight, outputWidth, outputHeight))
                return;
            if (!t.motionPipeline && !CreateTemporalPipeline())
                return;

            // Camera of this frame.
            std::array<double, 16> viewProj, inverseViewProj{};
            bool haveCamera = ChooseViewProj(viewProj) && Invert(viewProj, inverseViewProj);
            bool reset = t.lastEvaluated == UINT64_MAX || t.lastEvaluated + 1 != g_frame;
            if (haveCamera && t.havePrevious)
            {
                // A cut: the camera jumped (big change of the matrix).
                double difference = 0.0, norm = 0.0;
                for (int k = 0; k < 16; k++)
                {
                    difference = std::max(difference, fabs(viewProj[k] - t.previousViewProj[k]));
                    norm = std::max(norm, fabs(viewProj[k]));
                }
                reset |= difference > 0.5 * norm;
            }
            std::array<double, 16> previous = t.havePrevious ? t.previousViewProj : viewProj;
            if (haveCamera)
            {
                t.viewProj = viewProj;
                t.haveViewProj = true;
            }

            // Motion vectors.
            struct MotionConstants
            {
                float inverseViewProj[16];
                float previousViewProj[16];
                float renderSize[2];
                float zScale, zOffset;
                float velocityScale;
                uint32_t flags;
                float padding[2];
            } constants{};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    constants.inverseViewProj[r * 4 + c] = float(inverseViewProj[c * 4 + r]);
                    constants.previousViewProj[r * 4 + c] = float(previous[c * 4 + r]);
                }
            constants.renderSize[0] = float(renderWidth);
            constants.renderSize[1] = float(renderHeight);
            constants.zScale = fabsf(t.zScale) > 1e-6f ? t.zScale : 1.0f;
            constants.zOffset = t.zOffset;
            constants.velocityScale = t.velocityScale;
            constants.flags = (velocity && velocity->texture.resource ? 1u : 0u) | (haveCamera ? 2u : 0u);
            uint64_t constantOffset = AllocateUpload(AlignUp64(sizeof(constants), 256), 256);
            if (constantOffset == UINT64_MAX)
                return;
            memcpy(g_uploadCpu + constantOffset, &constants, sizeof(constants));
            uint32_t descriptors = AllocateDescriptors(2);
            Transition(depth->texture, STATE_SHADER_RESOURCE);
            g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptors), CpuSrv(depth->srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (constants.flags & 1u)
            {
                Transition(velocity->texture, STATE_SHADER_RESOURCE);
                g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptors + 1), CpuSrv(velocity->srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            else
            {
                g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptors + 1), CpuSrv(g_nullTextureSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            Transition(t.motion, D3D12_RESOURCE_STATE_RENDER_TARGET);
            BindDescriptorHeaps();
            g_list->SetGraphicsRootSignature(t.rootSignature.Get());
            g_list->SetGraphicsRootConstantBufferView(0, g_uploadGpu + constantOffset);
            g_list->SetGraphicsRootDescriptorTable(1, GpuHeapGpu(descriptors));
            g_list->SetPipelineState(t.motionPipeline.Get());
            g_list->OMSetRenderTargets(1, &t.motionRtv, FALSE, nullptr);
            D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(renderWidth), float(renderHeight), 0.0f, 1.0f };
            D3D12_RECT scissor{ 0, 0, LONG(renderWidth), LONG(renderHeight) };
            g_list->RSSetViewports(1, &viewport);
            g_list->RSSetScissorRects(1, &scissor);
            g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_list->DrawInstanced(3, 1, 0, 0);

            // Lighting remaster, on the scene at its own resolution (before the upscaler accumulates it).
            if (g_lightingOptions.Any() && haveCamera && t.showMotion == 0.0f)
            {
                Transition(t.motion, STATE_SHADER_RESOURCE);
                lighting::Frame frame;
                frame.list = g_list.Get();
                frame.color = { input.texture.resource.Get(), &input.texture.state };
                frame.colorRtv = input.rtv;
                frame.depth = { depth->texture.resource.Get(), &depth->texture.state };
                frame.motion = { t.motion.resource.Get(), &t.motion.state };
                frame.width = renderWidth;
                frame.height = renderHeight;
                frame.viewProj = viewProj;
                frame.zScale = constants.zScale;
                frame.zOffset = constants.zOffset;
                frame.reset = reset;
                frame.frame = g_frame;
                if (lighting::Apply(frame, g_lightingOptions))
                {
                    g_listNeedsHeaps = true;  // the lighting bound its own descriptor heap
                    color.lastWriteFrame = g_frame;
                }
            }
            if (!t.active)
            {
                t.lastEvaluated = g_frame;
                if (haveCamera)
                {
                    t.previousViewProj = viewProj;
                    t.havePrevious = true;
                }
                return;
            }

            // Upscale.
            Transition(t.motion, STATE_SHADER_RESOURCE);
            Transition(input.texture, STATE_SHADER_RESOURCE);
            Transition(t.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            float frameMs = t.lastTime.QuadPart ? float(double(now.QuadPart - t.lastTime.QuadPart) * 1000.0 / double(frequency.QuadPart)) : 33.3f;
            t.lastTime = now;
            upscaler::Frame frame;
            frame.list = g_list.Get();
            frame.color = input.texture.resource.Get();
            frame.depth = depth->texture.resource.Get();
            frame.motion = t.motion.resource.Get();
            frame.output = t.output.resource.Get();
            frame.renderWidth = renderWidth;
            frame.renderHeight = renderHeight;
            frame.outputWidth = outputWidth;
            frame.outputHeight = outputHeight;
            frame.jitterX = t.jitterX;
            frame.jitterY = t.jitterY;
            frame.reset = reset;
            frame.depthInverted = t.zScale < 0.0f;
            settings::Values options = settings::Get();
            frame.sharpness = float(options.sharpness) / 100.0f;
            frame.frameTimeMs = std::clamp(frameMs, 1.0f, 200.0f);
            if (t.haveViewProj)
                CameraParameters(t.viewProj, frame.cameraNear, frame.cameraFar, frame.fovY);
            frame.quality = settings::AtStartup().upscalerQuality;
            if (t.evaluations < 3 || (t.evaluations % 900) == 0)
                Log("[gpu] temporal frame %llu: camera %s (%zu candidates), z = %g * ndc + %g, velocity K %g, near %g far %g fov %.1f, jitter %.3f %.3f%s",
                    (unsigned long long)g_frame, haveCamera ? "ok" : "missing", t.candidates.size(), t.zScale, t.zOffset, t.velocityScale,
                    frame.cameraNear, frame.cameraFar, frame.fovY * 57.29578f, frame.jitterX, frame.jitterY, reset ? ", reset" : "");
            bool ok;
            if (t.showMotion != 0.0f)
            {
                // Debug view instead of the upscaler (the output keeps the render size's corner).
                ResolveSurface& view = EnsureSurface(color, g_renderScale);
                uint32_t viewDescriptors = AllocateDescriptors(2);
                g_device->CopyDescriptorsSimple(1, GpuHeapCpu(viewDescriptors), CpuSrv(g_nullTextureSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R16G16_FLOAT;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(t.motion.resource.Get(), &srv, GpuHeapCpu(viewDescriptors));
                g_device->CopyDescriptorsSimple(1, GpuHeapCpu(viewDescriptors + 1),
                                                CpuSrv((constants.flags & 1u) ? velocity->srvIndex : g_nullTextureSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                float viewConstants[64] = { t.showMotion };
                uint64_t viewOffset = AllocateUpload(256, 256);
                memcpy(g_uploadCpu + viewOffset, viewConstants, 256);
                Transition(view.texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                g_list->SetGraphicsRootSignature(t.rootSignature.Get());
                g_list->SetGraphicsRootConstantBufferView(0, g_uploadGpu + viewOffset);
                g_list->SetGraphicsRootDescriptorTable(1, GpuHeapGpu(viewDescriptors));
                g_list->SetPipelineState(t.motionViewPipeline.Get());
                g_list->OMSetRenderTargets(1, &view.rtv, FALSE, nullptr);
                g_list->DrawInstanced(3, 1, 0, 0);
                color.current = 0;
                t.evaluations++;
                t.lastEvaluated = g_frame;
                if (haveCamera)
                {
                    t.previousViewProj = viewProj;
                    t.havePrevious = true;
                }
                return;
            }
            LARGE_INTEGER evaluateStart, evaluateEnd;
            QueryPerformanceCounter(&evaluateStart);
            ok = upscaler::Evaluate(frame);
            QueryPerformanceCounter(&evaluateEnd);
            t.evaluateMs += double(evaluateEnd.QuadPart - evaluateStart.QuadPart) * 1000.0 / double(frequency.QuadPart);
            g_listNeedsHeaps = true;  // the upscaler bound its own descriptor heaps
            if (!ok)
            {
                t.failures++;
                return;
            }
            t.evaluations++;
            t.lastEvaluated = g_frame;
            if (haveCamera)
            {
                t.previousViewProj = viewProj;
                t.havePrevious = true;
            }

            // The output replaces the scene color for the rest of the frame.
            ResolveSurface& result = EnsureSurface(color, g_renderScale);
            Transition(t.output, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(result.texture, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = t.output.resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = result.texture.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{ 0, 0, 0, std::min(outputWidth, result.hostWidth), std::min(outputHeight, result.hostHeight), 1 };
            g_list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
            color.current = 0;
            color.lastWriteFrame = g_frame;
        }

        // Before a 2D draw: if it reads this frame's scene color, upscale the scene first.
        void UpscaleBeforeReading(uint32_t textureSlots, const TextureBinding* const* bindings)
        {
            if (!ScenePassActive() || g_temporal.done || !g_temporal.color)
                return;
            for (uint32_t fc = 0; fc < textureSlots; fc++)
            {
                if (!bindings[fc])
                    continue;
                uint32_t base = Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 6 + 1) & 0xFFFFF000;
                if (FindResolveTarget(base) == g_temporal.color)
                {
                    RunTemporal();
                    return;
                }
            }
        }

        // End of a frame: the next one's jitter, and forget this one's scene.
        void NextTemporalFrame(uint64_t nextFrame)
        {
            Temporal& t = g_temporal;
            t.color = t.depth = t.velocity = nullptr;
            t.done = false;
            t.candidates.clear();
            if (!t.active)
                return;
            upscaler::Jitter(nextFrame, ScaledSize(1280, g_sceneScale), ScaledSize(1280, g_renderScale), t.jitterX, t.jitterY);
            t.jitterGuestX = t.jitterX / g_sceneScale;
            t.jitterGuestY = t.jitterY / g_sceneScale;
        }

        // Predicated tiling and mid-frame copies of the scene (NOTES step 16, "faixa no gelo"). Each
        // tile copies its scene into the same texture before the refraction pass (resolve without
        // clear) and ends with the final scene (resolve with clear), so when tile 3's refraction
        // samples left of its first column it reads tile 2's final pixels, ice included: the ice is
        // applied twice in a band at the seam (the console does the same copies, in the same order).
        // So: when a later tile makes its first copy, keep the finals aside (`final`) and put back the
        // earlier tiles' last copies (`pre`); at that tile's final resolve, restore the finals.
        struct TileStash
        {
            TrackedResource pre, final;
            D3D12_RECT preRect{};          // host rectangle of the last final resolve
            uint64_t midFrame = UINT64_MAX, preFrame = UINT64_MAX, swapFrame = UINT64_MAX;
            bool swapped = false;
        };
        std::unordered_map<const ResolveSurface*, TileStash> g_tileStash;

        void CopySurfaceRegion(TrackedResource& to, TrackedResource& from, const D3D12_RECT* rect)
        {
            Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(to, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = from.resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = to.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{};
            if (rect)
                box = D3D12_BOX{ UINT(rect->left), UINT(rect->top), 0, UINT(rect->right), UINT(rect->bottom), 1 };
            g_list->CopyTextureRegion(&destination, rect ? UINT(rect->left) : 0, rect ? UINT(rect->top) : 0, 0, &source, rect ? &box : nullptr);
        }

        // A copy-only texture like `surface`, holding its current contents when created.
        void EnsureStashTexture(TrackedResource& stash, ResolveSurface& surface)
        {
            D3D12_RESOURCE_DESC desc = surface.texture.resource->GetDesc();
            if (stash.resource)
            {
                D3D12_RESOURCE_DESC old = stash.resource->GetDesc();
                if (old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format)
                    return;
            }
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            stash.resource.Reset();
            stash.state = D3D12_RESOURCE_STATE_COPY_DEST;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, stash.state, nullptr, IID_PPV_ARGS(&stash.resource))))
                Fatal("[gpu] could not create a %llux%u tile stash", (unsigned long long)desc.Width, desc.Height);
            CopySurfaceRegion(stash, surface.texture, nullptr);
        }

        // Before a color resolve into `surface` covering the host rectangle `rect`.
        void TileStashBeforeResolve(ResolveSurface& surface, const D3D12_RECT& rect, bool tileEnd, uint32_t base)
        {
            static const bool disabled = getenv("CRASH_GPU_NO_TILE_STASH") != nullptr;  // debug: the old behavior
            if (disabled || rect.left >= rect.right || rect.top >= rect.bottom)
                return;
            TileStash& s = g_tileStash[&surface];
            if (s.swapped && s.swapFrame != g_frame)
            {
                // A tile that never ended (should not happen): give the finals back anyway.
                CopySurfaceRegion(surface.texture, s.final, nullptr);
                s.swapped = false;
            }
            if (tileEnd)
            {
                if (s.midFrame == g_frame)
                {
                    EnsureStashTexture(s.pre, surface);
                    CopySurfaceRegion(s.pre, surface.texture, &rect);  // this tile's last copy
                    s.preFrame = g_frame;
                    s.preRect = rect;
                }
                if (s.swapped)
                    CopySurfaceRegion(surface.texture, s.final, nullptr);  // the earlier tiles' finals
                s.swapped = false;
                return;
            }
            s.midFrame = g_frame;
            if (s.preFrame == g_frame && !s.swapped && (rect.left != s.preRect.left || rect.top != s.preRect.top))
            {
                static uint32_t logged;
                if (logged++ < 4)
                    Log("[gpu] tile stash: copies into 0x%08X at (%ld,%ld) see the earlier tiles' copies, not their finals", base, rect.left, rect.top);
                EnsureStashTexture(s.final, surface);
                CopySurfaceRegion(s.final, surface.texture, nullptr);
                CopySurfaceRegion(surface.texture, s.pre, nullptr);
                s.swapped = true;
                s.swapFrame = g_frame;
            }
        }

        void Resolve()
        {
            InvalidateDrawState();
            g_stats.resolves++;
            uint32_t copyControl = Reg(xenos::REG_RB_COPY_CONTROL);
            uint32_t sourceSelect = copyControl & 7;
            bool colorClear = (copyControl >> 8) & 1;
            bool depthClear = (copyControl >> 9) & 1;
            uint32_t copyCommand = (copyControl >> 20) & 3;
            uint32_t surfaceInfo = Reg(xenos::REG_RB_SURFACE_INFO);
            uint32_t pitch = surfaceInfo & 0x3FFF;
            uint32_t msaa = (surfaceInfo >> 16) & 3;
            uint32_t samplesX = SamplesX(msaa);
            if (!pitch)
                return;

            // Rectangle: D3D9 puts the three vertices in vertex fetch constant 0.
            uint32_t fetch0 = Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0), fetch1 = Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + 1);
            if ((fetch0 & 3) != 3)
                return;
            uint32_t vertexAddress = gpu::GuestFromGpuAddress(fetch0 & 0xFFFFFFFC);
            const uint32_t* raw = reinterpret_cast<const uint32_t*>(g_memory.Translate(vertexAddress));
            float halfPixel = (Reg(xenos::REG_PA_SU_VTX_CNTL) & 1) ? 0.0f : 0.5f;
            int32_t fixed[6];
            for (int i = 0; i < 6; i++)
            {
                uint32_t bits = xenos::GpuSwap(raw[i], fetch1 & 3);
                float value;
                memcpy(&value, &bits, 4);
                fixed[i] = ToFixed16p8(value + halfPixel);
            }
            int32_t x0 = (std::min({ fixed[0], fixed[2], fixed[4] }) + 127) >> 8;
            int32_t y0 = (std::min({ fixed[1], fixed[3], fixed[5] }) + 127) >> 8;
            int32_t x1 = (std::max({ fixed[0], fixed[2], fixed[4] }) + 127) >> 8;
            int32_t y1 = (std::max({ fixed[1], fixed[3], fixed[5] }) + 127) >> 8;
            if ((Reg(xenos::REG_PA_SU_SC_MODE_CNTL) >> 16) & 1)
            {
                uint32_t offset = Reg(xenos::REG_PA_SC_WINDOW_OFFSET);
                int32_t ox = xenos::SignedBits(offset, 0, 15), oy = xenos::SignedBits(offset, 16, 15);
                x0 += ox; x1 += ox; y0 += oy; y1 += oy;
            }
            D3D12_RECT scissor = GuestScissor(16383, 16383);
            x0 = std::clamp(x0, int32_t(scissor.left), int32_t(scissor.right));
            x1 = std::clamp(x1, int32_t(scissor.left), int32_t(scissor.right));
            y0 = std::clamp(y0, int32_t(scissor.top), int32_t(scissor.bottom));
            y1 = std::clamp(y1, int32_t(scissor.top), int32_t(scissor.bottom));
            x0 &= ~7; y0 &= ~7;
            x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
            int32_t pitchAligned = int32_t(pitch & ~7u);
            x0 = std::min(x0, pitchAligned);
            x1 = std::min(x1, pitchAligned);
            y1 = std::min(y1, int32_t(RENDER_TARGET_HEIGHT));
            if (x0 >= x1 || y0 >= y1)
                return;

            bool isDepth = sourceSelect >= 4;
            uint32_t depthInfo = Reg(xenos::REG_RB_DEPTH_INFO);
            RenderTarget* source;
            if (isDepth)
                source = &GetRenderTarget(true, depthInfo & 0xFFF, pitch, (depthInfo >> 16) & 1, msaa);
            else
            {
                uint32_t colorInfo = Reg(xenos::ColorInfoRegister(sourceSelect));
                source = &GetRenderTarget(false, colorInfo & 0xFFF, pitch, (colorInfo >> 16) & 0xF, msaa);
            }

            float rowScale = HostRowScale(*source, msaa);
            float scale = source->scale;

            static uint32_t loggedResolves;
            bool logThis = loggedResolves < 40 || TracingSwap();

            // Copy.
            uint32_t destBase = Reg(xenos::REG_RB_COPY_DEST_BASE) & 0x1FFFFFFF;
            if ((copyCommand == uint32_t(xenos::CopyCommand::Raw) || copyCommand == uint32_t(xenos::CopyCommand::Convert)) && destBase)
            {
                uint32_t destPitchReg = Reg(xenos::REG_RB_COPY_DEST_PITCH);
                uint32_t destInfo = Reg(xenos::REG_RB_COPY_DEST_INFO);
                uint32_t destPitch = destPitchReg & 0x3FFF;
                uint32_t destHeight = (destPitchReg >> 16) & 0x3FFF;
                uint32_t destFormat = (destInfo >> 7) & 0x3F;
                int32_t expBias = xenos::SignedBits(destInfo, 16, 6);
                bool swapRedBlue = (destInfo >> 24) & 1;
                uint32_t bytesPerPixel;
                DXGI_FORMAT format = isDepth ? DXGI_FORMAT_R32_FLOAT : ResolveDestFormat(destFormat, bytesPerPixel);
                if (isDepth)
                    bytesPerPixel = 4;
                uint32_t pitchAligned32 = (std::max(destPitch, uint32_t(x1)) + 31) & ~31u;

                // Find the texture this resolve writes to (see Xenia's xenos.h comment on resolve
                // destination offsetting): pixel (x, y) goes to destBase + Tiled2D(x, y), so a
                // destination inside an existing target is offset by whole 32x32 macro tiles.
                int32_t dx = 0, dy = 0;
                ResolveTarget* destination = nullptr;
                ResolveTarget*& target = destination;
                uint32_t macroTileBytes = 32 * 32 * bytesPerPixel;
                for (auto& [base, candidate] : g_resolveTargets)
                {
                    if (candidate.format != format || candidate.width != pitchAligned32)
                        continue;
                    int64_t delta = int64_t(destBase) - int64_t(base);
                    if (delta == 0)
                    {
                        target = &candidate;
                        break;
                    }
                    if (delta % macroTileBytes)
                        continue;
                    int64_t tiles = delta / int64_t(macroTileBytes);
                    int64_t tilesPerRow = candidate.width / 32;
                    int64_t row = tiles >= 0 ? tiles / tilesPerRow : -((-tiles + tilesPerRow - 1) / tilesPerRow);
                    int64_t column = tiles - row * tilesPerRow;
                    for (int k = -1; k <= 1 && !target; k++)
                    {
                        int64_t ox = (column + k * tilesPerRow) * 32, oy = (row - k) * 32;
                        if (x0 + ox >= 0 && x1 + ox <= int64_t(candidate.width) && y0 + oy >= 0 && y1 + oy <= int64_t(candidate.height))
                        {
                            target = &candidate;
                            dx = int32_t(ox);
                            dy = int32_t(oy);
                        }
                    }
                    if (target)
                        break;
                }
                if (!target)
                {
                    // New texture: assume the rectangle's (32-aligned) top-left is its origin.
                    int32_t originX = x0 & ~31, originY = y0 & ~31;
                    uint32_t textureBase = destBase + uint32_t((originY / 32) * int32_t(pitchAligned32 / 32) + originX / 32) * macroTileBytes;
                    uint32_t height = destHeight ? std::max(destHeight, uint32_t(y1 - originY)) : uint32_t(y1 - originY);
                    target = &CreateResolveTarget(textureBase & 0x1FFFFFFF, format, bytesPerPixel, pitchAligned32, std::min(height, 8192u), destFormat);
                    dx = -originX;
                    dy = -originY;
                }

                if (logThis)
                {
                    Log("[gpu] resolve %s %u (EDRAM %u, format %u) rect (%d,%d)-(%d,%d) -> 0x%08X pitch %u height %u format %u exp %d swap %u, at (%d,%d), clear c%u d%u",
                        isDepth ? "depth" : "color", sourceSelect, (isDepth ? depthInfo : Reg(xenos::ColorInfoRegister(sourceSelect))) & 0xFFF,
                        source->guestFormat, x0, y0, x1, y1, destBase, destPitch, destHeight, destFormat, expBias, uint32_t(swapRedBlue), dx, dy,
                        uint32_t(colorClear), uint32_t(depthClear));
                    loggedResolves++;
                }

                ResolveSurface* surface = &EnsureSurface(*destination, scale);
                destination->current = SurfaceIndex(scale);
                NoteSceneResolve(*destination, *source, isDepth, sourceSelect);
                if (!isDepth)
                {
                    D3D12_RECT hostRect{ std::max(0L, ScaledFloor(float(x0 + dx), scale)), std::max(0L, ScaledFloor(float(y0 + dy), scale)),
                                         std::min(LONG(surface->hostWidth), ScaledCeil(float(x1 + dx), scale)), std::min(LONG(surface->hostHeight), ScaledCeil(float(y1 + dy), scale)) };
                    TileStashBeforeResolve(*surface, hostRect, colorClear, destBase);
                }
                ID3D12PipelineState* pipeline = GetResolvePipeline(format);
                if (pipeline)
                {
                    uint32_t descriptor = AllocateDescriptors(1);
                    Transition(source->texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    Transition(surface->texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    BindDescriptorHeaps();
                    g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptor), CpuSrv(source->srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    g_list->SetGraphicsRootSignature(g_blitRootSignature.Get());
                    g_list->SetGraphicsRootDescriptorTable(0, GpuHeapGpu(descriptor));
                    float expScale = ldexpf(1.0f, expBias);
                    uint32_t constants[4] = { uint32_t(ScaledFloor(float(dx), scale)), uint32_t(ScaledFloor(float(dy), scale)), 0,
                                              uint32_t(swapRedBlue) | (samplesX == 2 ? 4u : 0u) | RowScaleFlags(rowScale) };
                    memcpy(&constants[2], &expScale, 4);
                    g_list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
                    g_list->SetPipelineState(pipeline);
                    g_list->OMSetRenderTargets(1, &surface->rtv, FALSE, nullptr);
                    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(surface->hostWidth), float(surface->hostHeight), 0.0f, 1.0f };
                    D3D12_RECT rect{ std::max(0L, ScaledFloor(float(x0 + dx), scale)), std::max(0L, ScaledFloor(float(y0 + dy), scale)),
                                     std::min(LONG(surface->hostWidth), ScaledCeil(float(x1 + dx), scale)), std::min(LONG(surface->hostHeight), ScaledCeil(float(y1 + dy), scale)) };
                    if (rect.left < rect.right && rect.top < rect.bottom)
                    {
                        g_list->RSSetViewports(1, &viewport);
                        g_list->RSSetScissorRects(1, &rect);
                        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        g_list->DrawInstanced(3, 1, 0, 0);
                    }
                    target->lastWriteFrame = g_frame;
                }

                if (isDepth && target->format == DXGI_FORMAT_R32_FLOAT)
                {
                    if (ID3D12PipelineState* packedPipeline = GetResolveDepthPackedPipeline())
                    {
                        EnsurePackedDepth(*surface);
                        uint32_t descriptor = AllocateDescriptors(2);
                        Transition(source->texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        Transition(surface->packed, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        BindDescriptorHeaps();
                        g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptor), CpuSrv(source->srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptor + 1), CpuSrv(source->stencilSrvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        g_list->SetGraphicsRootSignature(g_blit2RootSignature.Get());
                        g_list->SetGraphicsRootDescriptorTable(0, GpuHeapGpu(descriptor));
                        bool isFloat = source->guestFormat == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
                        uint32_t constants[4] = { uint32_t(ScaledFloor(float(dx), scale)), uint32_t(ScaledFloor(float(dy), scale)), 0,
                                                  (isFloat ? 2u : 0u) | (samplesX == 2 ? 4u : 0u) | RowScaleFlags(rowScale) };
                        g_list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
                        g_list->SetPipelineState(packedPipeline);
                        g_list->OMSetRenderTargets(1, &surface->packedRtv, FALSE, nullptr);
                        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(surface->hostWidth), float(surface->hostHeight), 0.0f, 1.0f };
                        D3D12_RECT rect{ std::max(0L, ScaledFloor(float(x0 + dx), scale)), std::max(0L, ScaledFloor(float(y0 + dy), scale)),
                                         std::min(LONG(surface->hostWidth), ScaledCeil(float(x1 + dx), scale)), std::min(LONG(surface->hostHeight), ScaledCeil(float(y1 + dy), scale)) };
                        if (rect.left < rect.right && rect.top < rect.bottom)
                        {
                            g_list->RSSetViewports(1, &viewport);
                            g_list->RSSetScissorRects(1, &rect);
                            g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                            g_list->DrawInstanced(3, 1, 0, 0);
                        }
                    }
                }
            }
            else if (logThis)
            {
                Log("[gpu] resolve without copy (command %u, dest 0x%08X) rect (%d,%d)-(%d,%d), clear c%u d%u", copyCommand, destBase, x0, y0, x1, y1,
                    uint32_t(colorClear), uint32_t(depthClear));
                loggedResolves++;
            }

            // Clears (in host pixels).
            D3D12_RECT clearRect{ ScaledFloor(float(x0 * int32_t(samplesX)), scale), ScaledFloor(floorf(float(y0) * rowScale), scale),
                                  ScaledCeil(float(x1 * int32_t(samplesX)), scale), ScaledCeil(ceilf(float(y1) * rowScale), scale) };
            if (colorClear && !isDepth)
            {
                uint32_t value = Reg(xenos::REG_RB_COLOR_CLEAR);
                uint32_t valueLo = Reg(xenos::REG_RB_COLOR_CLEAR_LO);
                float color[4] = {};
                switch (xenos::ColorRenderTargetFormat(source->guestFormat))
                {
                case xenos::ColorRenderTargetFormat::k_8_8_8_8:
                case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
                    for (int i = 0; i < 4; i++)
                        color[i] = float((value >> (8 * i)) & 0xFF) / 255.0f;
                    if (source->guestFormat == uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA))
                    {
                        for (int i = 0; i < 3; i++)
                            color[i] = color[i] <= 0.04045f ? color[i] / 12.92f : powf((color[i] + 0.055f) / 1.055f, 2.4f);
                    }
                    break;
                case xenos::ColorRenderTargetFormat::k_2_10_10_10:
                case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
                    color[0] = float(value & 0x3FF) / 1023.0f;
                    color[1] = float((value >> 10) & 0x3FF) / 1023.0f;
                    color[2] = float((value >> 20) & 0x3FF) / 1023.0f;
                    color[3] = float(value >> 30) / 3.0f;
                    break;
                case xenos::ColorRenderTargetFormat::k_32_FLOAT:
                    memcpy(&color[0], &value, 4);
                    break;
                case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
                case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
                {
                    uint64_t bits = uint64_t(value) | (uint64_t(valueLo) << 32);
                    for (int i = 0; i < 4; i++)
                    {
                        uint16_t half = uint16_t(bits >> (16 * i));
                        uint32_t sign = (half >> 15) & 1, exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
                        float f = exponent ? ldexpf(1.0f + mantissa / 1024.0f, int(exponent) - 15) : ldexpf(mantissa / 1024.0f, -14);
                        color[i] = sign ? -f : f;
                    }
                    break;
                }
                default:
                    break;
                }
                Transition(source->texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                g_list->ClearRenderTargetView(source->view, color, 1, &clearRect);
                g_stats.clears++;
            }
            if (depthClear)
            {
                RenderTarget& depth = GetRenderTarget(true, depthInfo & 0xFFF, pitch, (depthInfo >> 16) & 1, msaa);
                uint32_t value = Reg(xenos::REG_RB_DEPTH_CLEAR);
                float depthValue;
                if (depth.guestFormat == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8))
                {
                    uint32_t f24 = value >> 8, exponent = f24 >> 20, mantissa = f24 & 0xFFFFF;
                    depthValue = exponent ? ldexpf(1.0f + mantissa / 1048576.0f, int(exponent) - 15) : ldexpf(mantissa / 1048576.0f, -14);
                }
                else
                {
                    depthValue = float(value >> 8) / 16777215.0f;
                }
                Transition(depth.texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                g_list->ClearDepthStencilView(depth.view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, std::clamp(depthValue, 0.0f, 1.0f),
                                              UINT8(value & 0xFF), 1, &clearRect);
                g_stats.clears++;
            }
        }

        // Draw ----------------------------------------------------------------------------------

        struct IndexData
        {
            std::vector<uint32_t> indices;
            uint32_t minIndex = UINT32_MAX;
            uint32_t maxIndex = 0;
            bool hasCut = false;
        };

        // Expands the guest indices (DMA, immediate or auto) to final 32-bit vertex indices with
        // the offset/clamp/reset rules applied, then converts the primitive type for D3D12.
        bool BuildIndices(const DrawPacket& packet, uint32_t primitive, uint32_t sourceSelect, bool index32, uint32_t count,
                          IndexData& out, D3D_PRIMITIVE_TOPOLOGY& topology, D3D12_PRIMITIVE_TOPOLOGY_TYPE& topologyType, bool& rectList)
        {
            static std::vector<uint32_t> raw;
            uint32_t resetIndex = Reg(xenos::REG_VGT_MULTI_PRIM_IB_RESET_INDX) & 0xFFFFFF;
            bool resetEnabled = (Reg(xenos::REG_PA_SU_SC_MODE_CNTL) >> 21) & 1;
            uint32_t indexOffset = Reg(xenos::REG_VGT_INDX_OFFSET) & 0xFFFFFF;
            uint32_t minVertex = Reg(xenos::REG_VGT_MIN_VTX_INDX) & 0xFFFFFF;
            uint32_t maxVertex = Reg(xenos::REG_VGT_MAX_VTX_INDX) & 0xFFFFFF;
            if (maxVertex < minVertex)
                maxVertex = 0xFFFFFF;

            // One pass per draw: read (and swap) each guest index, apply the reset index, offset
            // and clamp, and track the range. Without a reset index the loop has no branches and
            // vectorizes (a draw has hundreds to thousands of indices, ~1,400 draws per frame).
            constexpr uint32_t CUT = 0xFFFFFFFF;
            uint32_t minIndex = UINT32_MAX, maxIndex = 0;
            bool hasCut = false;
            auto expand = [&](uint32_t* destination, auto&& load) __attribute__((always_inline)) {
                if (!resetEnabled)
                {
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint32_t value = std::clamp((load(i) + indexOffset) & 0xFFFFFF, minVertex, maxVertex);
                        destination[i] = value;
                        minIndex = std::min(minIndex, value);
                        maxIndex = std::max(maxIndex, value);
                    }
                    return;
                }
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t value = load(i);
                    if ((value & 0xFFFFFF) == resetIndex)
                    {
                        destination[i] = CUT;
                        hasCut = true;
                        continue;
                    }
                    value = std::clamp((value + indexOffset) & 0xFFFFFF, minVertex, maxVertex);
                    destination[i] = value;
                    minIndex = std::min(minIndex, value);
                    maxIndex = std::max(maxIndex, value);
                }
            };

            if (sourceSelect == uint32_t(xenos::SourceSelect::DMA))
            {
                uint32_t elementSize = index32 ? 4 : 2;
                uint32_t available = packet.indexSize & 0xFFFFFF;
                if (available < count)
                    count = available;
                raw.resize(count);
                uint32_t endian = packet.indexSize >> 30;
                uint32_t address = gpu::GuestFromGpuAddress(packet.indexBase & ~(elementSize - 1));
                const uint8_t* source = g_memory.Translate(address);
                if (index32)
                {
                    const uint32_t* source32 = reinterpret_cast<const uint32_t*>(source);
                    switch (endian & 3)
                    {
                    case 0: expand(raw.data(), [&](uint32_t i) { return source32[i]; }); break;
                    case 2: expand(raw.data(), [&](uint32_t i) { return __builtin_bswap32(source32[i]); }); break;
                    default: expand(raw.data(), [&](uint32_t i) { return xenos::GpuSwap(source32[i], endian); }); break;
                    }
                }
                else
                {
                    const uint16_t* source16 = reinterpret_cast<const uint16_t*>(source);
                    if (endian == 1 || endian == 2)
                        expand(raw.data(), [&](uint32_t i) { return uint32_t(__builtin_bswap16(source16[i])); });
                    else
                        expand(raw.data(), [&](uint32_t i) { return uint32_t(source16[i]); });
                }
            }
            else if (sourceSelect == uint32_t(xenos::SourceSelect::Immediate))
            {
                raw.resize(count);
                expand(raw.data(), [&](uint32_t i) {
                    uint32_t dword = index32 ? i : i / 2;
                    uint32_t value = dword < packet.immediateDwords ? __builtin_bswap32(packet.immediateIndices[dword]) : 0;
                    return index32 ? value : ((i & 1) ? (value >> 16) : (value & 0xFFFF));
                });
            }
            else
            {
                raw.resize(count);
                resetEnabled = false;
                expand(raw.data(), [&](uint32_t i) { return i; });
            }
            g_work.indices += count;
            out.minIndex = minIndex;
            out.maxIndex = maxIndex;
            out.hasCut = hasCut;

            auto& indices = out.indices;
            indices.clear();
            rectList = false;
            switch (xenos::PrimitiveType(primitive))
            {
            case xenos::PrimitiveType::PointList:
                topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::LineList:
                topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::LineStrip:
                topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                indices = raw;
                break;
            case xenos::PrimitiveType::LineLoop:
                topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                indices = raw;
                if (count)
                    indices.push_back(raw[0]);
                break;
            case xenos::PrimitiveType::TriangleList:
            case xenos::PrimitiveType::TriangleWithWFlags:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::TriangleStrip:
            case xenos::PrimitiveType::QuadStrip:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::TriangleFan:
            case xenos::PrimitiveType::Polygon:
            {
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                uint32_t start = 0;
                for (uint32_t i = 0; i <= count; i++)
                {
                    if (i == count || raw[i] == CUT)
                    {
                        for (uint32_t j = start + 1; j + 1 < i; j++)
                            indices.insert(indices.end(), { raw[start], raw[j], raw[j + 1] });
                        start = i + 1;
                    }
                }
                out.hasCut = false;
                break;
            }
            case xenos::PrimitiveType::QuadList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                for (uint32_t i = 0; i + 3 < count; i += 4)
                    indices.insert(indices.end(), { raw[i], raw[i + 1], raw[i + 2], raw[i], raw[i + 2], raw[i + 3] });
                break;
            case xenos::PrimitiveType::RectangleList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                rectList = true;
                break;
            default:
            {
                static uint32_t logged;
                if (logged++ < 8)
                    Log("[gpu] unsupported primitive type %u", primitive);
                return false;
            }
            }
            if (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST || topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ||
                topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST)
                out.hasCut = false;
            return !indices.empty() && out.minIndex <= out.maxIndex;
        }

        // Coarse CPU profile of the command processor thread (CRASH_GPU_PROFILE): QPC ticks per
        // section of DrawImpl/Resolve, logged with the pacing stats.
        const char* const PROFILE_NAMES[] = { "state/shaders/index build", "vertex upload", "system constants", "textures", "constant buffers",
                                              "pipeline", "descriptors", "index upload", "resolve", "record" };
        uint64_t g_profileTicks[10];
        uint64_t g_profileLast;
        // Timed one frame in four (Swap sets it; the log scales by 4): ~11 QueryPerformanceCounter
        // per draw were ~1% of the thread.
        bool g_profileFrame = true;
        void ProfileStart()
        {
            if (!g_profileFrame)
                return;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            g_profileLast = uint64_t(now.QuadPart);
        }
        void ProfileMark(int bucket)
        {
            if (!g_profileFrame)
                return;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            g_profileTicks[bucket] += uint64_t(now.QuadPart) - g_profileLast;
            g_profileLast = uint64_t(now.QuadPart);
        }

        void DrawImpl(const DrawPacket& packet)
        {
            uint32_t initiator = packet.drawInitiator;
            uint32_t primitive = initiator & 0x3F;
            uint32_t sourceSelect = (initiator >> 6) & 3;
            bool index32 = (initiator >> 11) & 1;
            uint32_t count = initiator >> 16;

            uint32_t edramMode = Reg(xenos::REG_RB_MODECONTROL) & 7;
            if (edramMode == uint32_t(xenos::EdramMode::Copy))
            {
                {
                    ProfileStart();
                    Resolve();
                    ProfileMark(8);
                }
                return;
            }
            g_stats.draws++;
            g_work.draws++;
            ProfileStart();
            bool trace = (g_traceDraws > 0 && g_stats.draws >= g_traceFrom) || TracingSwap();
            if (trace)
            {
                if (g_traceDraws)
                    g_traceDraws--;
                Log("[gpu] draw #%llu: prim %u source %u count %u index32 %u edram %u vs %016llx ps %016llx surface 0x%08X color0 0x%08X depth 0x%08X depthctl 0x%08X mask 0x%04X vte 0x%X clip 0x%X mode 0x%X",
                    (unsigned long long)g_stats.draws, primitive, sourceSelect, count, uint32_t(index32), edramMode,
                    g_currentVertexShader ? (unsigned long long)g_currentVertexShader->info.hash : 0ull,
                    g_currentPixelShader ? (unsigned long long)g_currentPixelShader->info.hash : 0ull,
                    Reg(xenos::REG_RB_SURFACE_INFO), Reg(xenos::REG_RB_COLOR_INFO), Reg(xenos::REG_RB_DEPTH_INFO), Reg(xenos::REG_RB_DEPTHCONTROL),
                    Reg(xenos::REG_RB_COLOR_MASK), Reg(xenos::REG_PA_CL_VTE_CNTL), Reg(xenos::REG_PA_CL_CLIP_CNTL), Reg(xenos::REG_PA_SU_SC_MODE_CNTL));
                Log("[gpu]   stencil refmask 0x%08X bf 0x%08X, color1-3 0x%08X 0x%08X 0x%08X, ps writes 0x%X", Reg(xenos::REG_RB_STENCILREFMASK), Reg(xenos::REG_RB_STENCILREFMASK_BF),
                    Reg(xenos::ColorInfoRegister(1)), Reg(xenos::ColorInfoRegister(2)), Reg(xenos::ColorInfoRegister(3)),
                    g_currentPixelShader ? g_currentPixelShader->info.colorTargetsWritten : 0u);
                Log("[gpu]   bool consts %08X %08X %08X %08X | %08X %08X %08X %08X",
                    Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031), Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 1),
                    Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 2), Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 3),
                    Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 4), Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 5),
                    Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 6), Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + 7));
                Log("[gpu]   viewport scale %g %g %g offset %g %g %g, window offset 0x%08X scissor 0x%08X-0x%08X, blend0 0x%08X, colorctl 0x%08X",
                    RegF(xenos::REG_PA_CL_VPORT_XSCALE), RegF(xenos::REG_PA_CL_VPORT_YSCALE), RegF(xenos::REG_PA_CL_VPORT_ZSCALE),
                    RegF(xenos::REG_PA_CL_VPORT_XOFFSET), RegF(xenos::REG_PA_CL_VPORT_YOFFSET), RegF(xenos::REG_PA_CL_VPORT_ZOFFSET),
                    Reg(xenos::REG_PA_SC_WINDOW_OFFSET), Reg(xenos::REG_PA_SC_WINDOW_SCISSOR_TL), Reg(xenos::REG_PA_SC_WINDOW_SCISSOR_BR),
                    Reg(xenos::REG_RB_BLENDCONTROL0), Reg(xenos::REG_RB_COLORCONTROL));
            }
            if (edramMode != uint32_t(xenos::EdramMode::ColorDepth) && edramMode != uint32_t(xenos::EdramMode::DepthOnly))
            {
                Skip(SKIP_EDRAM_MODE);
                return;
            }
            uint32_t vizQuery = Reg(xenos::REG_PA_SC_VIZ_QUERY);
            if ((vizQuery & 1) && ((vizQuery >> 7) & 1))
            {
                Skip(SKIP_VIZ_QUERY);
                return;
            }
            if (!count)
                return;

            ShaderEntry* vs = PrepareShader(g_currentVertexShader);
            bool depthOnly = edramMode == uint32_t(xenos::EdramMode::DepthOnly);
            ShaderEntry* ps = depthOnly ? nullptr : PrepareShader(g_currentPixelShader);
            if (!vs || (!depthOnly && !ps))
            {
                Skip(SKIP_SHADER);
                return;
            }
            // CRASH_GPU_SKIP_PS=hash1,hash2: drop the draws of those pixel shaders (to find which
            // pass draws what).
            static const char* skipList = getenv("CRASH_GPU_SKIP_PS");
            if (skipList && ps)
            {
                char hash[17];
                snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)ps->info.hash);
                if (strstr(skipList, hash))
                {
                    Skip(SKIP_SHADER);
                    return;
                }
            }

            uint32_t modeControl = Reg(xenos::REG_PA_SU_SC_MODE_CNTL);
            bool cullFront = modeControl & 1, cullBack = (modeControl >> 1) & 1;

            static IndexData indexData;
            indexData.indices.clear();  // keeps its capacity (BuildIndices sets the rest)
            D3D_PRIMITIVE_TOPOLOGY topology;
            D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType;
            bool rectList;
            if (!BuildIndices(packet, primitive, sourceSelect, index32, count, indexData, topology, topologyType, rectList))
            {
                Skip(SKIP_PRIMITIVE);
                return;
            }
            bool polygonal = topologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            if (polygonal && cullFront && cullBack)
            {
                Skip(SKIP_CULL);
                return;
            }

            // Render targets.
            uint32_t surfaceInfo = Reg(xenos::REG_RB_SURFACE_INFO);
            uint32_t pitch = surfaceInfo & 0x3FFF;
            uint32_t msaa = (surfaceInfo >> 16) & 3;
            uint32_t samplesX = SamplesX(msaa);
            if (!pitch)
            {
                Skip(SKIP_NO_TARGET);
                return;
            }
            uint32_t colorMaskReg = Reg(xenos::REG_RB_COLOR_MASK);
            RenderTarget* colorTargets[4] = {};
            uint32_t renderTargetCount = 0;
            uint32_t colorMask = 0;
            float expBias[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            if (ps)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t mask = (colorMaskReg >> (4 * i)) & 0xF;
                    if (!(ps->info.colorTargetsWritten & (1u << i)) || !mask)
                        continue;
                    uint32_t info = Reg(xenos::ColorInfoRegister(i));
                    colorTargets[i] = &GetRenderTarget(false, info & 0xFFF, pitch, (info >> 16) & 0xF, msaa);
                    colorMask |= mask << (4 * i);
                    expBias[i] = ldexpf(1.0f, xenos::SignedBits(info, 20, 6));
                    renderTargetCount = i + 1;
                }
            }
            uint32_t depthControl = Reg(xenos::REG_RB_DEPTHCONTROL);
            RenderTarget* depthTarget = nullptr;
            uint32_t depthInfo = Reg(xenos::REG_RB_DEPTH_INFO);
            if (depthControl & 3)
                depthTarget = &GetRenderTarget(true, depthInfo & 0xFFF, pitch, (depthInfo >> 16) & 1, msaa);
            if (!renderTargetCount && !depthTarget)
            {
                Skip(SKIP_NO_TARGET);
                return;
            }
            // Host rows per guest row (one viewport for all targets of the draw), and the scale
            // of the draw (scene or 2D, see g_sceneScale).
            float rowScale = 1.0f;
            float scale = g_renderScale;
            bool sceneDraw = false;
            {
                RenderTarget* reference = depthTarget;
                for (uint32_t i = 0; i < renderTargetCount && !reference; i++)
                    reference = colorTargets[i];
                rowScale = HostRowScale(*reference, msaa);
                scale = reference->scale;
                sceneDraw = reference->IsScene();
                static uint32_t loggedMismatch;
                for (uint32_t i = 0; i < renderTargetCount; i++)
                {
                    if (colorTargets[i] && colorTargets[i]->rowSamples != reference->rowSamples && loggedMismatch++ < 8)
                        Log("[gpu] draw mixes render targets with different row layouts (EDRAM %u and the depth/first target)", Reg(xenos::ColorInfoRegister(i)) & 0xFFF);
                }
            }
            // Guest pixels (viewport transform, scissor) vs the host target (samplesX times wider).
            uint32_t targetWidth = pitch, targetHeight = RENDER_TARGET_HEIGHT;
            uint32_t hostWidth = pitch * samplesX;

            // Depth-alias clears. D3D9 on the 360 clears color render targets fast by drawing a
            // rectangle into a *depth* surface over the same EDRAM (4x MSAA, so each pixel covers
            // 2x2 samples), with ZFUNC ALWAYS and the color packed into depth and stencil: the
            // EDRAM word of D24S8 is (depth24 << 8) | stencil, read back by the color view as
            // R = stencil, G/B/A = depth bytes. Crash clears its velocity map (EDRAM 1080) like
            // this in every tile. Host depth and color targets are separate, so such a draw is
            // also drawn into the k_8_8_8_8 color target at that base, with a pixel shader that
            // writes those bytes. (Depth tiles swap their 40-sample halves in EDRAM; that doesn't
            // matter for the flat values these clears write.)
            RenderTarget* aliasedColor = nullptr;
            uint32_t aliasedMask = 0, aliasedStencil = 0;
            static const bool noDepthAlias = getenv("CRASH_GPU_NO_DEPTH_ALIAS") != nullptr;  // debug: the old behavior
            if (!renderTargetCount && depthTarget && !noDepthAlias)
            {
                bool depthWrite = ((depthControl >> 1) & 1) && ((depthControl >> 2) & 1) && ((depthControl >> 4) & 7) == 7;
                bool stencil = depthControl & 1;
                uint32_t stencilRefMask = Reg(xenos::REG_RB_STENCILREFMASK);
                bool stencilReplace = stencil && ((depthControl >> 8) & 7) == 7 && ((depthControl >> 14) & 7) == 2 && ((stencilRefMask >> 16) & 0xFF) == 0xFF;
                if (depthWrite && (!stencil || stencilReplace))
                {
                    aliasedColor = FindAliasedColorTarget(depthInfo & 0xFFF, hostWidth);
                    aliasedMask = stencilReplace ? 0xF : 0xE;
                    aliasedStencil = stencilRefMask & 0xFF;
                }
            }

            ProfileMark(0);
            // Upload: indices.
            uint64_t indexBytes = indexData.indices.size() * 4;
            uint64_t indexOffset = AllocateUpload(indexBytes, 16);
            if (indexOffset == UINT64_MAX)
                return;
            memcpy(g_uploadCpu + indexOffset, indexData.indices.data(), indexBytes);

            ProfileMark(7);
            // Upload: vertex data of each vertex fetch constant the vertex shader uses.
            SystemConstants system{};
            struct Range
            {
                uint32_t fetchConstant;
                uint32_t guestAddress;
                uint32_t startDwords;
                uint32_t dwords;
                uint32_t endian;
            };
            Range ranges[96];
            uint32_t rangeCount = 0;
            uint64_t vertexBytes = 0;
            for (const VertexFetchBinding& binding : vs->info.vertexFetches)
            {
                uint32_t fc = binding.fetchConstant;
                xenos::VertexFetch fetch{ { Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 2), Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 2 + 1) } };
                if (fetch.Type() != 3 || !fetch.SizeDwords())
                    continue;
                uint32_t size = fetch.SizeDwords();
                int64_t start = 0, end = size;
                if (binding.indexedByVertexIndex && binding.minOffsetDwords <= binding.maxEndDwords)
                {
                    start = int64_t(indexData.minIndex) * binding.strideDwords + binding.minOffsetDwords;
                    end = int64_t(indexData.maxIndex) * binding.strideDwords + binding.maxEndDwords;
                }
                start = std::clamp<int64_t>(start, 0, size);
                end = std::clamp<int64_t>(end, start, size);
                end = std::min<int64_t>(end, start + MAX_VERTEX_RANGE_BYTES / 4);
                uint32_t guestAddress = gpu::GuestFromGpuAddress(fetch.Address());
                if (uint64_t(fetch.Address() & 0x1FFFFFFF) + uint64_t(end) * 4 > PHYSICAL_SIZE)
                    continue;
                Range& range = ranges[rangeCount++];
                range = { fc, guestAddress, uint32_t(start), uint32_t(end - start), fetch.Endianness() };
                vertexBytes += AlignUp64(range.dwords * 4, 16);
            }
            uint64_t vertexOffset = 0;
            if (vertexBytes)
            {
                vertexOffset = AllocateUpload(vertexBytes, 256);
                if (vertexOffset == UINT64_MAX)
                    return;
                uint64_t cursor = 0;
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    const Range& range = ranges[r];
                    const uint32_t* source = reinterpret_cast<const uint32_t*>(g_memory.Translate(range.guestAddress)) + range.startDwords;
                    uint32_t* destination = reinterpret_cast<uint32_t*>(g_uploadCpu + vertexOffset + cursor);
                    // One loop per swap mode, so each vectorizes (GpuSwap switches per dword).
                    switch (range.endian & 3)
                    {
                    case 0: memcpy(destination, source, size_t(range.dwords) * 4); break;
                    case 1: for (uint32_t i = 0; i < range.dwords; i++) destination[i] = xenos::GpuSwap(source[i], 1); break;
                    case 2: for (uint32_t i = 0; i < range.dwords; i++) destination[i] = __builtin_bswap32(source[i]); break;
                    default: for (uint32_t i = 0; i < range.dwords; i++) destination[i] = xenos::GpuSwap(source[i], 3); break;
                    }
                    // Byte offset in the upload ring (one SRV over all of it, VERTEX_DATA_DESCRIPTOR).
                    system.vbBases[range.fetchConstant] = int32_t(vertexOffset + cursor) - int32_t(range.startDwords * 4);
                    cursor += AlignUp64(range.dwords * 4, 16);
                }
            }

            ProfileMark(1);
            // System constants.
            uint32_t vte = Reg(xenos::REG_PA_CL_VTE_CNTL);
            uint32_t clipControl = Reg(xenos::REG_PA_CL_CLIP_CNTL);
            float scaleX = (vte & 1) ? RegF(xenos::REG_PA_CL_VPORT_XSCALE) : 1.0f;
            float offsetX = (vte & 2) ? RegF(xenos::REG_PA_CL_VPORT_XOFFSET) : 0.0f;
            float scaleY = (vte & 4) ? RegF(xenos::REG_PA_CL_VPORT_YSCALE) : 1.0f;
            float offsetY = (vte & 8) ? RegF(xenos::REG_PA_CL_VPORT_YOFFSET) : 0.0f;
            float scaleZ = (vte & 16) ? RegF(xenos::REG_PA_CL_VPORT_ZSCALE) : 1.0f;
            float offsetZ = (vte & 32) ? RegF(xenos::REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
            float addX = 0.0f, addY = 0.0f;
            if ((modeControl >> 16) & 1)
            {
                uint32_t offset = Reg(xenos::REG_PA_SC_WINDOW_OFFSET);
                addX += float(xenos::SignedBits(offset, 0, 15));
                addY += float(xenos::SignedBits(offset, 16, 15));
            }
            if (!(Reg(xenos::REG_PA_SU_VTX_CNTL) & 1))
            {
                addX += 0.5f;
                addY += 0.5f;
            }
            float w = float(targetWidth), h = float(targetHeight), hostW = float(hostWidth);
            system.ndcScale[0] = scaleX * 2.0f / w;
            system.ndcOffset[0] = (offsetX + addX) * 2.0f / w - 1.0f;
            system.ndcScale[1] = -scaleY * 2.0f / h;
            system.ndcOffset[1] = 1.0f - (offsetY + addY) * 2.0f / h;
            system.ndcScale[2] = scaleZ;
            system.ndcOffset[2] = offsetZ;
            if ((vte >> 8) & 1)
                system.flags |= FLAG_XY_DIVIDED_BY_W;
            if ((vte >> 9) & 1)
                system.flags |= FLAG_Z_DIVIDED_BY_W;
            if ((vte >> 10) & 1)
                system.flags |= FLAG_W_NOT_RECIPROCAL;
            uint32_t programControl = Reg(xenos::REG_SQ_PROGRAM_CNTL);
            if ((programControl >> 18) & 1)
            {
                system.flags |= FLAG_PARAM_GEN;
                system.paramGenRegister = (Reg(xenos::REG_SQ_CONTEXT_MISC) >> 8) & 0xFF;
            }
            uint32_t colorControl = Reg(xenos::REG_RB_COLORCONTROL);
            if ((colorControl >> 3) & 1)
            {
                system.flags |= FLAG_ALPHA_TEST;
                system.alphaFunc = colorControl & 7;
                system.alphaRef[0] = RegF(xenos::REG_RB_ALPHA_REF);
            }
            memcpy(system.colorExpBias, expBias, sizeof(expBias));
            system.renderScale[0] = 1.0f / scale;
            for (uint32_t i = 0; i < 8; i++)
                system.boolConsts[i] = Reg(xenos::REG_SHADER_CONSTANT_BOOL_000_031 + i);
            for (uint32_t i = 0; i < 32; i++)
                system.loopConsts[i] = Reg(xenos::REG_SHADER_CONSTANT_LOOP_00 + i);
            // xe_flags.w, read only by DEPTH_ALIAS_PS: stencil value and 20e4 depth.
            if (aliasedColor)
                system.padding0 = aliasedStencil | (((depthInfo >> 16) & 1) ? 0x100u : 0u);

            ProfileMark(2);
            // Textures and samplers (vertex and pixel shader share the fetch constants).
            const TextureBinding* bindings[32] = {};
            uint32_t textureSlots = 0;
            for (ShaderEntry* shader : { vs, ps })
            {
                if (!shader)
                    continue;
                for (const TextureBinding& binding : shader->info.textures)
                {
                    if (!bindings[binding.fetchConstant])
                        bindings[binding.fetchConstant] = &binding;
                    textureSlots = std::max(textureSlots, binding.fetchConstant + 1);
                }
            }
            if (!sceneDraw)
                UpscaleBeforeReading(textureSlots, bindings);
            uint32_t textureSrvs[32];
            D3D12_SAMPLER_DESC samplers[32];
            g_drawIconPalette = 0;
            for (uint32_t fc = 0; fc < textureSlots; fc++)
            {
                textureSrvs[fc] = g_nullTextureSrv;
                samplers[fc] = D3D12_SAMPLER_DESC{ D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 1, D3D12_COMPARISON_FUNC_NEVER, {}, 0.0f, D3D12_FLOAT32_MAX };
                if (!bindings[fc])
                    continue;
                xenos::TextureFetch fetch;
                for (uint32_t k = 0; k < 6; k++)
                    fetch.dword[k] = Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 6 + k);
                float hdScale;
                textureSrvs[fc] = GetTextureSrv(fetch, &hdScale);
                ResolveTarget* fetchedTarget = FindResolveTarget(fetch.BaseAddress());
                samplers[fc] = MakeSampler(fetch, bindings[fc], !fetchedTarget);
                if (hdScale != 1.0f)
                    AdjustSamplerForHd(samplers[fc], hdScale);
                system.texSizeScale[fc] = fetchedTarget && fetchedTarget->Current().scale > 0.0f ? 1.0f / fetchedTarget->Current().scale : hdScale;
                system.texSwizzles[fc] = fetch.Swizzle();
                if (trace)
                {
                    ResolveTarget* resolved = FindResolveTarget(fetch.BaseAddress());
                    Log("[gpu]   tf%u: base 0x%08X format %u %ux%u pitch %u tiled %u swizzle 0x%03X filter fetch %u/%u/%u/%u instr %u/%u/%u/%u -> 0x%X%s", fc, fetch.BaseAddress(), fetch.Format(),
                        fetch.Width(), fetch.Height(), fetch.Pitch(), uint32_t(fetch.Tiled()), fetch.Swizzle(),
                        fetch.MagFilter(), fetch.MinFilter(), fetch.MipFilter(), fetch.AnisoFilter(),
                        bindings[fc]->magFilter, bindings[fc]->minFilter, bindings[fc]->mipFilter, bindings[fc]->anisoFilter, uint32_t(samplers[fc].Filter),
                        resolved ? (resolved->Current().packed.resource ? " (resolve target, has packed depth)" : " (resolve target)") : "");
                    if (resolved)
                        Log("[gpu]        resolve target %ux%u", resolved->width, resolved->height);
                    else if (fetch.Height() == 1 && fetch.Width() <= 256)
                    {
                        // Small 1D textures (palettes): the raw guest dwords.
                        const uint32_t* raw = reinterpret_cast<const uint32_t*>(g_memory.Translate(gpu::GuestFromGpuAddress(fetch.BaseAddress())));
                        std::string text;
                        for (uint32_t i = 0; i < std::min(fetch.Width(), 256u); i++)
                        {
                            char item[12];
                            snprintf(item, sizeof(item), " %08X", raw[i]);
                            text += item;
                        }
                        Log("[gpu]        endian %u, raw:%s", fetch.Endianness(), text.c_str());
                    }
                }
                system.texLodBias[fc] = float(fetch.LodBias()) / 32.0f;
            }
            // A replaced button icon brings its own palette (the pal*fp shaders' 256x1 texture, s8).
            if (g_drawIconPalette)
            {
                for (uint32_t fc = 0; fc < textureSlots; fc++)
                {
                    if (!bindings[fc])
                        continue;
                    xenos::TextureFetch fetch;
                    for (uint32_t k = 0; k < 6; k++)
                        fetch.dword[k] = Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 6 + k);
                    if (fetch.Width() == 256 && fetch.Height() == 1)
                        textureSrvs[fc] = g_drawIconPalette;
                }
            }

            ProfileMark(3);
            // Constant buffers.
            uint64_t systemOffset = AllocateUpload(AlignUp64(sizeof(SystemConstants), 256), 256);
            uint64_t vsConstOffset = AllocateUpload(4096, 256);
            uint64_t psConstOffset = AllocateUpload(4096, 256);
            if (systemOffset == UINT64_MAX || vsConstOffset == UINT64_MAX || psConstOffset == UINT64_MAX)
                return;
            memcpy(g_uploadCpu + systemOffset, &system, sizeof(system));
            for (int stage = 0; stage < 2; stage++)
            {
                uint32_t constReg = Reg(stage == 0 ? xenos::REG_SQ_VS_CONST : xenos::REG_SQ_PS_CONST);
                uint32_t base = constReg & 0x1FF;
                uint32_t* destination = reinterpret_cast<uint32_t*>(g_uploadCpu + (stage == 0 ? vsConstOffset : psConstOffset));
                const uint32_t* source = reinterpret_cast<const uint32_t*>(
                    g_memory.Translate(GPU_MMIO_BEGIN + (xenos::REG_SHADER_CONSTANT_000_X + base * 4) * 4));
                uint32_t available = base * 4 < 2048 ? std::min(1024u, 2048u - base * 4) : 0u;
                for (uint32_t i = 0; i < available; i++)
                    destination[i] = __builtin_bswap32(source[i]);
                for (uint32_t i = available; i < 1024; i++)
                    destination[i] = 0;
                if (stage == 0 && sceneDraw && ScenePassActive() && available >= 1024)
                    NoteSceneCamera(vs->info.hash, source, system.ndcScale[2], system.ndcOffset[2]);
                if (trace)
                {
                    // Non-zero shader constants of a traced draw (vertex shader: c0-c31 only).
                    std::string line;
                    const float* values = reinterpret_cast<const float*>(destination);
                    for (uint32_t c = 0; c < (stage == 1 ? 256u : 32u); c++)
                    {
                        if (!destination[c * 4] && !destination[c * 4 + 1] && !destination[c * 4 + 2] && !destination[c * 4 + 3])
                            continue;
                        char text[96];
                        snprintf(text, sizeof(text), " c%u=(%g %g %g %g)", c, values[c * 4], values[c * 4 + 1], values[c * 4 + 2], values[c * 4 + 3]);
                        line += text;
                    }
                    Log("[gpu]   %s constants (base %u):%s", stage == 1 ? "ps" : "vs", base, line.c_str());
                }
            }

            ProfileMark(4);
            // Pipeline.
            PipelineKey key{};
            key.vs = vs->info.hash;
            key.ps = ps ? ps->info.hash : 0;
            for (uint32_t i = 0; i < renderTargetCount; i++)
            {
                key.rtvFormats[i] = colorTargets[i] ? colorTargets[i]->rtvFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
                key.blend[i] = Reg(xenos::BlendControlRegister(i));
            }
            key.renderTargetCount = renderTargetCount;
            key.colorMask = colorMask;
            if (depthTarget)
            {
                key.dsvFormat = depthTarget->rtvFormat;
                key.depthControl = depthControl;
                key.stencilMasks = Reg(xenos::REG_RB_STENCILREFMASK) & 0xFFFF00;
            }
            if (polygonal)
            {
                // Window space, y down (see runtime/NOTES.md, "Chiratta de capacete"):
                // - PA_SU_SC_MODE_CNTL.face only orients culling: 0 = counter-clockwise is front,
                //   1 = clockwise (the 360's D3DCULL_CW is 2 = cull_back with face 0, D3DCULL_CCW is
                //   6 = cull_back with face 1). The game culls only mirrored characters (scientist
                //   chirattas, world matrix with determinant -1) with D3DCULL_CW; reading face 0 as
                //   "clockwise is front" culled their outside and drew them inside out.
                // - Two-sided stencil keeps D3D9's rule, clockwise = front ops, whatever the face
                //   bit (the shadow pass's clockwise full-screen quad uses the front ops with face 0).
                //   SV_IsFrontFace follows the stencil side.
                key.frontCounterClockwise = false;
                bool faceClockwise = ((modeControl >> 2) & 1) != 0;
                bool cullClockwise = faceClockwise ? cullFront : cullBack;
                bool cullCounterClockwise = faceClockwise ? cullBack : cullFront;
                key.cullMode = cullClockwise ? D3D12_CULL_MODE_FRONT : (cullCounterClockwise ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE);
                static const bool noCull = getenv("CRASH_GPU_NO_CULL") != nullptr;  // debug: no culling at all
                if (noCull)
                    key.cullMode = D3D12_CULL_MODE_NONE;
                if ((modeControl >> 11) & 3)
                {
                    float scale = RegF(xenos::REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
                    float offset = RegF(xenos::REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
                    key.slopeBias = scale * (1.0f / 16.0f);
                    key.depthBias = int32_t(offset * 16777216.0f);
                }
            }
            else
            {
                key.cullMode = D3D12_CULL_MODE_NONE;
            }
            key.depthClip = !((clipControl >> 16) & 1);
            key.topologyType = topologyType;
            key.rectList = rectList;
            key.stripCut = indexData.hasCut;
            ID3D12PipelineState* pipeline = GetPipeline(key, vs, ps);
            if (!pipeline)
            {
                Skip(SKIP_PIPELINE);
                return;
            }
            ID3D12PipelineState* aliasPipeline = nullptr;
            if (aliasedColor)
            {
                PipelineKey aliasKey = key;
                aliasKey.ps = DEPTH_ALIAS_PS_KEY;
                aliasKey.rtvFormats[0] = aliasedColor->rtvFormat;
                aliasKey.blend[0] = 0x00010001;
                aliasKey.renderTargetCount = 1;
                aliasKey.colorMask = aliasedMask;
                aliasKey.dsvFormat = 0;
                aliasKey.depthControl = 0;
                aliasKey.stencilMasks = 0;
                aliasKey.depthBias = 0;
                aliasKey.slopeBias = 0.0f;
                aliasPipeline = GetPipeline(aliasKey, vs, nullptr, g_depthAliasPs.Get());
            }
            if (trace)
            {
                D3D12_RECT traced = GuestScissor(targetWidth, targetHeight);
                Log("[gpu]   drawn: %zu indices (range %u-%u), %u targets, depth %s%s, scissor (%d,%d)-(%d,%d), ndc scale %g %g offset %g %g, %u vertex ranges",
                    indexData.indices.size(), indexData.minIndex, indexData.maxIndex, renderTargetCount, depthTarget ? "yes" : "no",
                    aliasPipeline ? " (+ aliased color target)" : "",
                    int(traced.left), int(traced.top), int(traced.right), int(traced.bottom),
                    system.ndcScale[0], system.ndcScale[1], system.ndcOffset[0], system.ndcOffset[1], rangeCount);
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    const float* data = reinterpret_cast<const float*>(g_uploadCpu) + (system.vbBases[ranges[r].fetchConstant] / 4 + ranges[r].startDwords);
                    Log("[gpu]   vf%u at 0x%08X endian %u: %g %g %g %g %g %g %g", ranges[r].fetchConstant, ranges[r].guestAddress, ranges[r].endian,
                        data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
                    const uint32_t* bits = reinterpret_cast<const uint32_t*>(data);
                    Log("[gpu]     hex: %08X %08X %08X %08X %08X %08X | %08X %08X %08X %08X %08X %08X", bits[0], bits[1], bits[2], bits[3], bits[4], bits[5],
                        bits[6], bits[7], bits[8], bits[9], bits[10], bits[11]);
                    // CRASH_GPU_TRACE_DUMP=1: also the vertex data (host order, from the first
                    // fetched dword) as draw_<n>_vf<fc>.bin next to the screenshots.
                    static const bool dumpVertices = getenv("CRASH_GPU_TRACE_DUMP") != nullptr;
                    if (dumpVertices)
                    {
                        char fileName[64];
                        snprintf(fileName, sizeof(fileName), "draw_%llu_vf%u.bin", (unsigned long long)g_stats.draws, ranges[r].fetchConstant);
                        std::ofstream(g_screenshotDir / fileName, std::ios::binary).write(reinterpret_cast<const char*>(data), std::streamsize(ranges[r].dwords) * 4);
                    }
                }
            }

            ProfileMark(5);
            // Descriptors: textures (t0..), vertex data (t0 space1), samplers.
            // Tables are declared with 32 entries; only the used ones are written.
            constexpr uint32_t srvCount = 32;
            uint32_t descriptors = AllocateDescriptors(srvCount);
            for (uint32_t i = 0; i < textureSlots; i++)
                g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptors + i), CpuSrv(textureSrvs[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            constexpr uint32_t samplerCount = 32;
            uint64_t samplerKey = HashBytes(samplers, sizeof(D3D12_SAMPLER_DESC) * textureSlots, samplerCount);
            uint32_t samplerBase;
            auto samplerIt = g_samplerBlocks.find(samplerKey);
            if (samplerIt != g_samplerBlocks.end())
            {
                samplerBase = samplerIt->second;
            }
            else
            {
                if (g_samplerCount + samplerCount > SAMPLER_DESCRIPTORS)
                {
                    WaitIdle();
                    g_samplerBlocks.clear();
                    g_samplerCount = 0;
                }
                samplerBase = g_samplerCount;
                g_samplerCount += samplerCount;
                for (uint32_t i = 0; i < samplerCount; i++)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += size_t(samplerBase + i) * g_samplerSize;
                    D3D12_SAMPLER_DESC desc = i < textureSlots ? samplers[i] : D3D12_SAMPLER_DESC{ D3D12_FILTER_MIN_MAG_MIP_POINT,
                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 1,
                        D3D12_COMPARISON_FUNC_NEVER, {}, 0.0f, D3D12_FLOAT32_MAX };
                    g_device->CreateSampler(&desc, handle);
                }
                g_samplerBlocks[samplerKey] = samplerBase;
            }

            // Targets.
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
            for (uint32_t i = 0; i < renderTargetCount; i++)
            {
                if (colorTargets[i])
                {
                    Transition(colorTargets[i]->texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    rtvs[i] = colorTargets[i]->view;
                }
                else
                {
                    rtvs[i] = g_nullRtv;
                }
            }
            if (depthTarget)
                Transition(depthTarget->texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);

            ProfileMark(6);
            // Record.
            BindDescriptorHeaps();
            DrawListState& state = g_drawState;
            bool fresh = !state.valid || state.rootSignature != g_drawRootSignature.Get();
            if (fresh)
            {
                // A (re)set root signature clears the root arguments: everything below is recorded.
                state = DrawListState{};
                state.rootSignature = g_drawRootSignature.Get();
                g_list->SetGraphicsRootSignature(state.rootSignature);
                D3D12_INDEX_BUFFER_VIEW ringView{ g_uploadGpu, uint32_t(UPLOAD_RING_SIZE), DXGI_FORMAT_R32_UINT };
                g_list->IASetIndexBuffer(&ringView);  // the whole upload ring; draws start at their indices
            }
            g_list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, g_uploadGpu + systemOffset);
            g_list->SetGraphicsRootConstantBufferView(ROOT_VS_CONSTANTS, g_uploadGpu + vsConstOffset);
            g_list->SetGraphicsRootConstantBufferView(ROOT_PS_CONSTANTS, g_uploadGpu + psConstOffset);
            g_list->SetGraphicsRootDescriptorTable(ROOT_TEXTURES, GpuHeapGpu(descriptors));
            if (fresh)
                g_list->SetGraphicsRootDescriptorTable(ROOT_VERTEX_DATA, GpuHeapGpu(VERTEX_DATA_DESCRIPTOR));
            D3D12_GPU_DESCRIPTOR_HANDLE samplerHandle = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
            samplerHandle.ptr += uint64_t(samplerBase) * g_samplerSize;
            if (fresh || state.samplerTable != samplerHandle.ptr)
            {
                g_list->SetGraphicsRootDescriptorTable(ROOT_SAMPLERS, samplerHandle);
                state.samplerTable = samplerHandle.ptr;
            }
            graphics::EffectConstants effects = g_effects;
            if (sceneDraw && g_temporal.active)
            {
                // Sub-pixel jitter of the scene in NDC of this target (rectangle lists are clears).
                if (!rectList)
                {
                    effects.jitter[0] = 2.0f * g_temporal.jitterGuestX / float(targetWidth);
                    effects.jitter[1] = -2.0f * g_temporal.jitterGuestY / (float(targetHeight) * rowScale);
                }
                // Texture LOD bias of upscaling: mip selection for the output resolution.
                effects.textureLodScale = g_sceneScale / g_renderScale;
            }
            if (fresh || memcmp(&state.effects, &effects, sizeof(effects)) != 0)
            {
                g_list->SetGraphicsRoot32BitConstants(ROOT_EFFECTS, graphics::EFFECT_CONSTANT_DWORDS, &effects, 0);
                state.effects = effects;
            }
            if (fresh || state.pipeline != pipeline)
            {
                g_list->SetPipelineState(pipeline);
                state.pipeline = pipeline;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = depthTarget ? depthTarget->view : D3D12_CPU_DESCRIPTOR_HANDLE{};
            if (fresh || state.renderTargetCount != renderTargetCount || state.dsv.ptr != dsv.ptr ||
                memcmp(state.rtvs, rtvs, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * renderTargetCount) != 0)
            {
                g_list->OMSetRenderTargets(renderTargetCount, renderTargetCount ? rtvs : nullptr, FALSE, depthTarget ? &depthTarget->view : nullptr);
                state.renderTargetCount = renderTargetCount;
                memcpy(state.rtvs, rtvs, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * renderTargetCount);
                state.dsv = dsv;
            }
            float blendFactor[4] = { RegF(xenos::REG_RB_BLEND_RED), RegF(xenos::REG_RB_BLEND_RED + 1), RegF(xenos::REG_RB_BLEND_RED + 2),
                                     RegF(xenos::REG_RB_BLEND_RED + 3) };
            if (fresh || memcmp(state.blendFactor, blendFactor, sizeof(blendFactor)) != 0)
            {
                g_list->OMSetBlendFactor(blendFactor);
                memcpy(state.blendFactor, blendFactor, sizeof(blendFactor));
            }
            uint32_t stencilRef = Reg(xenos::REG_RB_STENCILREFMASK) & 0xFF;
            if (fresh || state.stencilRef != stencilRef)
            {
                g_list->OMSetStencilRef(stencilRef);
                state.stencilRef = stencilRef;
            }
            D3D12_VIEWPORT viewport{ 0.0f, 0.0f, hostW * scale, h * rowScale * scale, 0.0f, 1.0f };
            if (fresh || memcmp(&state.viewport, &viewport, sizeof(viewport)) != 0)
            {
                g_list->RSSetViewports(1, &viewport);
                state.viewport = viewport;
            }
            D3D12_RECT scissor = GuestScissor(targetWidth, targetHeight);
            scissor.left = ScaledFloor(float(scissor.left * LONG(samplesX)), scale);
            scissor.right = ScaledCeil(float(scissor.right * LONG(samplesX)), scale);
            scissor.top = ScaledFloor(floorf(float(scissor.top) * rowScale), scale);
            scissor.bottom = std::min(ScaledCeil(ceilf(float(scissor.bottom) * rowScale), scale), LONG(ScaledSize(RENDER_TARGET_HEIGHT, scale)));
            if (fresh || memcmp(&state.scissor, &scissor, sizeof(scissor)) != 0)
            {
                g_list->RSSetScissorRects(1, &scissor);
                state.scissor = scissor;
            }
            if (fresh || state.topology != topology)
            {
                g_list->IASetPrimitiveTopology(topology);
                state.topology = topology;
            }
            state.valid = true;
            uint32_t firstIndex = uint32_t(indexOffset / 4);
            g_list->DrawIndexedInstanced(uint32_t(indexData.indices.size()), 1, firstIndex, 0, 0);
            if (aliasPipeline)
            {
                Transition(aliasedColor->texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                g_list->SetPipelineState(aliasPipeline);
                g_list->OMSetRenderTargets(1, &aliasedColor->view, FALSE, nullptr);
                g_list->DrawIndexedInstanced(uint32_t(indexData.indices.size()), 1, firstIndex, 0, 0);
                g_stats.aliasClears++;
                InvalidateDrawState();
            }
            ProfileMark(9);
        }

        // Presentation --------------------------------------------------------------------------

        void CreateBackBuffers()
        {
            for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++)
            {
                g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
                g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, g_backBufferRtvs[i]);
            }
        }

        void ResizeIfNeeded()
        {
            uint32_t width, height;
            window::GetClientSize(width, height);
            if (!width || !height || (width == g_backBufferWidth && height == g_backBufferHeight))
                return;
            WaitIdle();
            for (auto& buffer : g_backBuffers)
                buffer.Reset();
            if (SUCCEEDED(g_swapChain->ResizeBuffers(BACK_BUFFER_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                  g_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
            {
                g_backBufferWidth = width;
                g_backBufferHeight = height;
            }
            CreateBackBuffers();
        }

        // The anti-aliasing targets: RGBA8 at the size of the front buffer, recreated when it changes.
        bool EnsureAaTargets(uint32_t width, uint32_t height)
        {
            if (g_aaInput.texture.resource && g_aaWidth == width && g_aaHeight == height)
                return true;
            if (g_aaInput.texture.resource)
                WaitIdle();
            for (PostTarget* target : { &g_aaInput, &g_aaOutput })
            {
                target->texture = {};
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = width;
                desc.Height = height;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, STATE_SHADER_RESOURCE, nullptr,
                                                             IID_PPV_ARGS(&target->texture.resource))))
                {
                    Log("[gpu] could not create the anti-aliasing targets %ux%u", width, height);
                    g_aaInput.texture = {};
                    g_aaOutput.texture = {};
                    return false;
                }
                target->texture.state = STATE_SHADER_RESOURCE;
                g_device->CreateRenderTargetView(target->texture.resource.Get(), nullptr, target->rtv);
                g_device->CreateShaderResourceView(target->texture.resource.Get(), nullptr, CpuSrv(target->srvIndex));
            }
            g_aaWidth = width;
            g_aaHeight = height;
            return true;
        }

        // video.window = fullscreen: exclusive swap chain fullscreen while the window has the
        // focus (DXGI drops it on Alt+Tab; the borderless window stays, and it is retried).
        void UpdateFullscreen()
        {
            static bool s_maybeFullscreen;
            bool wanted = window::Mode() == settings::WindowMode::Fullscreen;
            if (!wanted && !s_maybeFullscreen)
                return;
            s_maybeFullscreen = wanted;
            BOOL fullscreen = FALSE;
            g_swapChain->GetFullscreenState(&fullscreen, nullptr);
            static ULONGLONG s_lastAttempt;
            if (wanted && !fullscreen && GetForegroundWindow() == g_hwnd && GetTickCount64() - s_lastAttempt > 2000)
            {
                s_lastAttempt = GetTickCount64();
                HRESULT result = g_swapChain->SetFullscreenState(TRUE, nullptr);
                Log("[gpu] exclusive fullscreen: %s (0x%08X)", SUCCEEDED(result) ? "on" : "failed, staying borderless", uint32_t(result));
            }
            else if (!wanted && fullscreen)
            {
                g_swapChain->SetFullscreenState(FALSE, nullptr);
                Log("[gpu] exclusive fullscreen: off");
            }
        }

        void SaveScreenshot(ID3D12Resource* backBuffer, const std::filesystem::path& path)
        {
            D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
            uint64_t totalBytes;
            g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
            D3D12_RESOURCE_DESC bufferDesc{};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = totalBytes;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            ComPtr<ID3D12Resource> readback;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback))))
                return;
            TransitionRaw(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = backBuffer;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            TransitionRaw(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            WaitIdle();
            void* data;
            D3D12_RANGE range{ 0, SIZE_T(totalBytes) };
            if (SUCCEEDED(readback->Map(0, &range, &data)))
            {
                std::vector<uint8_t> pixels(size_t(footprint.Footprint.RowPitch) * footprint.Footprint.Height);
                memcpy(pixels.data(), static_cast<uint8_t*>(data) + footprint.Offset, pixels.size());
                for (size_t i = 0; i < pixels.size(); i += 4)
                    pixels[i + 3] = 255;
                readback->Unmap(0, nullptr);
                if (WritePng(path, pixels.data(), footprint.Footprint.Width, footprint.Footprint.Height, footprint.Footprint.RowPitch))
                    Log("[gpu] screenshot saved: %s", path.string().c_str());
                else
                    Log("[gpu] could not write %s", path.string().c_str());
            }
        }

        bool CreateRootSignatures()
        {
            // Draws.
            D3D12_DESCRIPTOR_RANGE textureRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, 0 };
            D3D12_DESCRIPTOR_RANGE vertexRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, 0 };
            D3D12_DESCRIPTOR_RANGE samplerRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 32, 0, 0, 0 };
            D3D12_ROOT_PARAMETER parameters[ROOT_PARAMETER_COUNT]{};
            parameters[ROOT_SYSTEM_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[ROOT_SYSTEM_CONSTANTS].Descriptor = { 0, 0 };
            parameters[ROOT_SYSTEM_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[ROOT_VS_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[ROOT_VS_CONSTANTS].Descriptor = { 1, 0 };
            parameters[ROOT_VS_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            parameters[ROOT_PS_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[ROOT_PS_CONSTANTS].Descriptor = { 2, 0 };
            parameters[ROOT_PS_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            parameters[ROOT_TEXTURES].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[ROOT_TEXTURES].DescriptorTable = { 1, &textureRange };
            parameters[ROOT_TEXTURES].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[ROOT_VERTEX_DATA].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[ROOT_VERTEX_DATA].DescriptorTable = { 1, &vertexRange };
            parameters[ROOT_VERTEX_DATA].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            parameters[ROOT_SAMPLERS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[ROOT_SAMPLERS].DescriptorTable = { 1, &samplerRange };
            parameters[ROOT_SAMPLERS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[ROOT_EFFECTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[ROOT_EFFECTS].Constants = { 3, 0, graphics::EFFECT_CONSTANT_DWORDS };
            parameters[ROOT_EFFECTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC desc{ ROOT_PARAMETER_COUNT, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

            auto create = [](const D3D12_ROOT_SIGNATURE_DESC& rootDesc, ComPtr<ID3D12RootSignature>& out) {
                ComPtr<ID3DBlob> blob, errors;
                if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
                {
                    Log("[gpu] root signature: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
                    return false;
                }
                return SUCCEEDED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)));
            };
            if (!create(desc, g_drawRootSignature))
                return false;

            // Resolve / present: one SRV, 4 root constants, a linear clamp static sampler.
            D3D12_DESCRIPTOR_RANGE blitRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER blitParameters[2]{};
            blitParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            blitParameters[0].DescriptorTable = { 1, &blitRange };
            blitParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            blitParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            blitParameters[1].Constants = { 0, 0, 8 };
            blitParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC blitDesc{ 2, blitParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            if (!create(blitDesc, g_blitRootSignature))
                return false;
            // Packed depth resolve: the same with two SRVs (depth, stencil).
            D3D12_DESCRIPTOR_RANGE blit2Range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
            blitParameters[0].DescriptorTable = { 1, &blit2Range };
            return create(blitDesc, g_blit2RootSignature);
        }

        // The settings screen changed the internal resolution or the upscaler (RequestReconfigure):
        // between two frames, with the GPU idle, the scales change, every render target and resolve
        // surface is recreated at its new size (keeping its descriptors; contents cleared: the game
        // redraws and resolves them, and the settings screen is only open in the frontend), the
        // upscaler is switched and its history dropped, and if the shaders' variant changes (720p
        // without an upscaler or HD textures is translated unscaled, see SetRenderScaled) they are
        // translated again and the pipelines rebuilt.
        std::atomic<bool> g_reconfigureRequested{ false };
        bool g_renderScaled = false;

        void Reconfigure()
        {
            settings::Values wanted = settings::Get();
            LARGE_INTEGER start, end, frequency;
            QueryPerformanceCounter(&start);
            WaitIdle();

            float renderScale = float(settings::ResolutionOf(wanted.renderHeight).height) / 720.0f;
            upscaler::Select(wanted.upscaler);
            bool temporal = upscaler::Active() != settings::Upscaler::Off;
            if (temporal && !g_temporal.motionPipeline && !CreateTemporalPipeline())
            {
                Log("[gpu] could not create the motion vector pass: temporal upscaling off");
                temporal = false;
            }
            float sceneScale = temporal ? renderScale * settings::UpscalerRatio(wanted.upscalerQuality) : renderScale;
            bool scaled = renderScale != 1.0f || sceneScale != 1.0f || hd::Active();
            float oldRender = g_renderScale, oldScene = g_sceneScale;
            g_renderScale = renderScale;
            g_sceneScale = sceneScale;

            // Temporal upscaling: no history across the change; its targets follow the sizes.
            Temporal& t = g_temporal;
            t.active = temporal;
            t.lastEvaluated = UINT64_MAX;
            t.havePrevious = t.haveViewProj = false;
            if (!temporal)
            {
                t.motion = {};
                t.output = {};
                t.renderWidth = t.renderHeight = t.outputWidth = t.outputHeight = 0;
            }
            g_tileStash.clear();

            size_t targets = 0, surfaces = 0;
            if (renderScale != oldRender || sceneScale != oldScene)
            {
                for (auto& [key, target] : g_renderTargets)
                {
                    CreateRenderTargetResource(target);
                    targets++;
                }
                for (auto& [base, target] : g_resolveTargets)
                {
                    // Surface 0 is at the output scale, 1 at the scene's when they differ.
                    bool had[2] = { target.surfaces[0].texture.resource != nullptr, target.surfaces[1].texture.resource != nullptr };
                    bool packed[2] = { target.surfaces[0].packed.resource != nullptr, target.surfaces[1].packed.resource != nullptr };
                    for (ResolveSurface& surface : target.surfaces)
                    {
                        surface.texture = {};
                        surface.packed = {};
                        surface.scale = 0.0f;
                    }
                    bool split = sceneScale != renderScale;
                    if (!split)
                    {
                        had[0] |= had[1], packed[0] |= packed[1];
                        had[1] = packed[1] = false;
                        target.current = 0;
                    }
                    for (int i = 0; i < 2; i++)
                    {
                        if (!had[i])
                            continue;
                        ResolveSurface& surface = EnsureSurface(target, i ? sceneScale : renderScale);
                        if (packed[i])
                            EnsurePackedDepth(surface);
                        surfaces++;
                    }
                    if (!target.Current().texture.resource)
                        target.current = target.surfaces[0].texture.resource ? 0 : 1;
                }
            }

            if (scaled != g_renderScaled)
            {
                SetRenderScaled(scaled);
                g_renderScaled = scaled;
                for (auto& [hash, shader] : g_shaders)
                {
                    shader->translated = false;
                    shader->failed = false;
                    shader->bytecode = nullptr;
                }
                g_pipelines.clear();
                Log("[gpu] shaders will be translated again (%s variant)", scaled ? "scaled" : "720p");
            }
            Submit();  // the clears of the new resources
            settings::RendererApplied(wanted.renderHeight, wanted.upscaler, wanted.upscalerQuality);

            QueryPerformanceCounter(&end);
            QueryPerformanceFrequency(&frequency);
            Log("[gpu] reconfigured: internal resolution %s (%.2fx), %s%s%s, scene %.3fx; %zu render targets and %zu resolve surfaces recreated in %.0f ms",
                settings::ResolutionOf(wanted.renderHeight).name, renderScale, temporal ? upscaler::ActiveName().c_str() : "no upscaler",
                temporal ? " " : "", temporal ? settings::UpscalerQualityName(wanted.upscalerQuality) : "", sceneScale, targets, surfaces,
                double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart));
        }

        void ParseScreenshotSettings()
        {
            if (const char* frames = getenv("CRASH_SCREENSHOTS"))
            {
                const char* p = frames;
                while (*p)
                {
                    char* end;
                    unsigned long long value = strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    g_screenshotFrames.push_back(value);
                    p = *end ? end + 1 : end;
                }
            }
            // CRASH_SCREENSHOT_TIMES=19.5,30: screenshots at those input-clock seconds (the clock of
            // CRASH_INPUT_SCRIPT), for runs whose frame rate isn't known in advance; or seconds after
            // a test event: gameplay_ready+2.5, level_ready#2+0 (settings/direct_boot.h).
            if (const char* times = getenv("CRASH_SCREENSHOT_TIMES"))
            {
                const char* p = times;
                while (*p)
                {
                    boot::Anchored time;
                    if (!boot::ParseAnchored(p, 1000.0, time))
                        break;
                    if (time.event.empty())
                        g_screenshotTimes.push_back(time.offsetMs);
                    else
                        g_screenshotAnchors.push_back(time);
                    p = *p ? p + 1 : p;
                }
                std::sort(g_screenshotTimes.begin(), g_screenshotTimes.end());
            }
            if (const char* dir = getenv("CRASH_SCREENSHOT_DIR"))
                g_screenshotDir = dir;
            else
                g_screenshotDir = std::filesystem::current_path();
            g_swapRedBlue = getenv("CRASH_PRESENT_SWAP_RB") != nullptr;
        }
    }

    bool Init()
    {
        if (getenv("CRASH_NO_RENDER"))
        {
            Log("[gpu] rendering disabled (CRASH_NO_RENDER)");
            return false;
        }
        g_renderScale = settings::RenderScale();
        g_sceneScale = g_renderScale;  // until the upscaler is known (after the device)
        if (g_renderScale != 1.0f)
            Log("[gpu] internal resolution %s (%.2fx the game's 1280x720)", settings::ResolutionOf(settings::AtStartup().renderHeight).name, g_renderScale);

        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        g_compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        g_createBlob = compiler ? reinterpret_cast<decltype(g_createBlob)>(GetProcAddress(compiler, "D3DCreateBlob")) : nullptr;
        if (!g_compile)
            Log("[gpu] d3dcompiler_47.dll not found: only cached shaders can be used");

        g_debugLayer = getenv("CRASH_D3D12_DEBUG") != nullptr;
        if (g_debugLayer)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
        }

        if (FAILED(CreateDXGIFactory2(g_debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&g_factory))) ||
            FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
        {
            Log("[gpu] no Direct3D 12 device: rendering disabled");
            return false;
        }
        if (g_debugLayer)
        {
            ComPtr<ID3D12InfoQueue> queue;
            if (SUCCEEDED(g_device.As(&queue)))
                queue->SetMuteDebugOutput(FALSE);
        }

        // Temporal upscaler: probed on this device; the scene scale depends on it.
        upscaler::Init(g_device.Get(), settings::AtStartup().upscaler, (vfs::UserDataDir() / "ngx").wstring());
        g_temporal.active = upscaler::Active() != settings::Upscaler::Off;
        g_sceneScale = g_temporal.active ? settings::SceneScale() : g_renderScale;
        // HD textures need the guest texel size in the shaders too (xe_tex_size_scale).
        hd::Init();
        g_renderScaled = g_renderScale != 1.0f || g_sceneScale != 1.0f || hd::Active();
        SetRenderScaled(g_renderScaled);
        if (g_temporal.active)
            Log("[gpu] %s %s: scene at %.3fx the game's 1280x720, output %.3fx", upscaler::ActiveName().c_str(),
                settings::UpscalerQualityName(settings::AtStartup().upscalerQuality), g_sceneScale, g_renderScale);

        D3D12_COMMAND_QUEUE_DESC queueDesc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue));
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator));
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator.Get(), nullptr, IID_PPV_ARGS(&g_list));

        // Descriptor heaps.
        auto makeHeap = [](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, ComPtr<ID3D12DescriptorHeap>& heap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{ type, count, shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
            return SUCCEEDED(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
        };
        if (!makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_DESCRIPTORS, false, g_rtvHeap) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_DESCRIPTORS, false, g_dsvHeap) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CPU_SRV_DESCRIPTORS, false, g_cpuSrvHeap) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SHADER_VISIBLE_DESCRIPTORS, true, g_gpuSrvHeap) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_DESCRIPTORS, true, g_samplerHeap))
        {
            Log("[gpu] could not create descriptor heaps");
            return false;
        }
        g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_dsvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        g_srvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_samplerSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        g_descriptorRing.Init(VERTEX_DATA_DESCRIPTOR);  // the last slot is the upload ring's SRV
        watch::Init();

        // Null descriptors.
        g_nullTextureSrv = AllocateCpuSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture{};
        nullTexture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullTexture.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(nullptr, &nullTexture, CpuSrv(g_nullTextureSrv));
        g_nullBufferSrv = AllocateCpuSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC nullBuffer{};
        nullBuffer.Format = DXGI_FORMAT_R32_TYPELESS;
        nullBuffer.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        nullBuffer.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullBuffer.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        g_device->CreateShaderResourceView(nullptr, &nullBuffer, CpuSrv(g_nullBufferSrv));
        g_nullRtv = AllocateRtv();
        D3D12_RENDER_TARGET_VIEW_DESC nullRtv{};
        nullRtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(nullptr, &nullRtv, g_nullRtv);

        // Upload ring.
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = UPLOAD_RING_SIZE;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        if (FAILED(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&g_uploadBuffer))))
        {
            Log("[gpu] could not create the upload buffer");
            return false;
        }
        D3D12_RANGE noRead{ 0, 0 };
        g_uploadBuffer->Map(0, &noRead, reinterpret_cast<void**>(&g_uploadCpu));
        g_uploadGpu = g_uploadBuffer->GetGPUVirtualAddress();
        g_uploadRing.Init(UPLOAD_RING_SIZE);
        {
            // One raw SRV over the whole upload ring, in the last slot of the shader-visible heap
            // (the descriptor ring stops before it): a draw's vertex data is found by its offset in
            // SystemConstants::vbBases, instead of a CreateShaderResourceView per draw (~7% of the
            // command processor thread in the village).
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = uint32_t(UPLOAD_RING_SIZE / 4);
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            g_device->CreateShaderResourceView(g_uploadBuffer.Get(), &srv, GpuHeapCpu(VERTEX_DATA_DESCRIPTOR));
        }

        g_shaderCacheDir = vfs::UserDataDir() / "shadercache";
        std::error_code error;
        std::filesystem::create_directories(g_shaderCacheDir, error);
        g_dumpShaders = getenv("CRASH_DUMP_SHADERS") != nullptr;

        if (!CreateRootSignatures())
        {
            Log("[gpu] could not create root signatures");
            return false;
        }
        g_rectListGs = CompileHlsl(RECT_LIST_GS, "gs_5_1", "rect_list_gs");
        g_blitVs = CompileHlsl(BLIT_VS, "vs_5_1", "blit_vs");
        g_resolvePs = CompileHlsl(RESOLVE_PS, "ps_5_1", "resolve_ps");
        g_resolveDepthPackedPs = CompileHlsl(RESOLVE_DEPTH_PACKED_PS, "ps_5_1", "resolve_depth_packed_ps");
        g_depthAliasPs = CompileHlsl(DEPTH_ALIAS_PS, "ps_5_1", "depth_alias_ps");
        g_presentPs = CompileHlsl(PRESENT_PS, "ps_5_1", "present_ps");
        g_fxaaPs = CompileHlsl(FXAA_PS, "ps_5_1", "fxaa_ps");
        if (!g_rectListGs || !g_blitVs || !g_resolvePs || !g_resolveDepthPackedPs || !g_depthAliasPs || !g_presentPs || !g_fxaaPs)
        {
            Log("[gpu] could not compile the built-in shaders");
            return false;
        }
        if (g_temporal.active && !CreateTemporalPipeline())
        {
            Log("[gpu] could not create the motion vector pass: temporal upscaling off");
            g_temporal.active = false;
            g_sceneScale = g_renderScale;
        }
        NextTemporalFrame(0);

        // Window and swap chain.
        window::SetMode(settings::Get().window);
        g_hwnd = HWND(window::Create(WINDOW_WIDTH, WINDOW_HEIGHT, L"Crash of the Titans (CrashRecomp)"));
        if (!g_hwnd)
        {
            Log("[gpu] could not create the window");
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = WINDOW_WIDTH;
        swapDesc.Height = WINDOW_HEIGHT;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = BACK_BUFFER_COUNT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        {
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            if (SUCCEEDED(g_factory.As(&factory5)) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                g_allowTearing = tearing != FALSE;
            if (g_allowTearing)
                swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            Log("[gpu] swap chain: tearing %s (frame rate above the monitor's refresh with V-Sync off, also when the window is not focused)",
                g_allowTearing ? "allowed" : "not supported");
        }
        ComPtr<IDXGISwapChain1> swapChain;
        if (FAILED(g_factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &swapDesc, nullptr, nullptr, &swapChain)) ||
            FAILED(swapChain.As(&g_swapChain)))
        {
            Log("[gpu] could not create the swap chain");
            return false;
        }
        g_factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++)
            g_backBufferRtvs[i] = AllocateRtv();
        CreateBackBuffers();

        // Present pipeline.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC present{};
        present.pRootSignature = g_blitRootSignature.Get();
        present.VS = { g_blitVs->GetBufferPointer(), g_blitVs->GetBufferSize() };
        present.PS = { g_presentPs->GetBufferPointer(), g_presentPs->GetBufferSize() };
        present.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        present.SampleMask = UINT_MAX;
        present.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        present.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        present.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        present.NumRenderTargets = 1;
        present.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        present.SampleDesc.Count = 1;
        if (FAILED(g_device->CreateGraphicsPipelineState(&present, IID_PPV_ARGS(&g_presentPipeline))))
        {
            Log("[gpu] could not create the present pipeline");
            return false;
        }
        present.PS = { g_fxaaPs->GetBufferPointer(), g_fxaaPs->GetBufferSize() };
        if (FAILED(g_device->CreateGraphicsPipelineState(&present, IID_PPV_ARGS(&g_fxaaPipeline))))
        {
            Log("[gpu] could not create the FXAA pipeline");
            return false;
        }
        for (PostTarget* target : { &g_aaInput, &g_aaOutput })
        {
            target->rtv = AllocateRtv();
            target->srvIndex = AllocateCpuSrv();
        }
        if (!smaa::Init(g_device.Get(), g_list.Get(),
                        [](const std::string& source, const char* target, const char* name) { return CompileHlsl(source, target, name); }))
            Log("[gpu] SMAA unavailable: video.antialiasing = smaa falls back to FXAA");
        if (!lighting::Init(g_device.Get(), g_queue.Get(),
                            [](const std::string& source, const char* target, const char* name) { return CompileHlsl(source, target, name); }))
            Log("[gpu] lighting remaster unavailable");

        ParseScreenshotSettings();
        overlay::Init(g_device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                      [](const std::string& source, const char* target, const char* name) { return CompileHlsl(source, target, name); },
                      g_screenshotDir);
        if (const char* trace = getenv("CRASH_GPU_TRACE"))
            g_traceDraws = uint32_t(strtoul(trace, nullptr, 10));
        if (const char* from = getenv("CRASH_GPU_TRACE_FROM"))
            g_traceFrom = strtoull(from, nullptr, 10);
        if (const char* swap = getenv("CRASH_GPU_TRACE_SWAP"))
            g_traceSwap = strtoull(swap, nullptr, 10);

        DXGI_ADAPTER_DESC1 adapterDesc{};
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(g_factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
            adapter->GetDesc1(&adapterDesc);
        char adapterName[128];
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
        Log("[gpu] Direct3D 12 renderer on %s%s; shader cache %s", adapterName, g_debugLayer ? " (debug layer)" : "",
            g_shaderCacheDir.string().c_str());
        DEVMODEA displayMode{};
        displayMode.dmSize = sizeof(displayMode);
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &displayMode))
            Log("[gpu] primary display: %lux%lu at %lu Hz", displayMode.dmPelsWidth, displayMode.dmPelsHeight, displayMode.dmDisplayFrequency);
        g_active = true;
        settings::menu::SetAvailable(true);
        return true;
    }

    bool IsActive()
    {
        return g_active;
    }

    int TestShaders(const std::filesystem::path& folder)
    {
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        g_compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        g_createBlob = compiler ? reinterpret_cast<decltype(g_createBlob)>(GetProcAddress(compiler, "D3DCreateBlob")) : nullptr;
        if (!g_compile)
        {
            Log("[gpu] d3dcompiler_47.dll not found");
            return 1;
        }
        g_shaderCacheDir = vfs::UserDataDir() / "shadercache";
        std::error_code error;
        std::filesystem::create_directories(g_shaderCacheDir, error);
        g_dumpShaders = true;

        int failures = 0, total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (entry.path().extension() != ".out")
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto be32 = [&](size_t offset) {
                return offset + 4 <= data.size() ? (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
                                                       (uint32_t(data[offset + 2]) << 8) | data[offset + 3]
                                                 : 0u;
            };
            // Container: magic 0x102A1100 (pixel) / 0x102A1101 (vertex), size of the header block
            // (the physical block follows it), ... The dword at 0x18 points at the shader
            // definition, whose first two dwords are the microcode offset in the physical block and
            // its size (literal constants come first).
            uint32_t magic = be32(0), definition = be32(0x18);
            uint32_t offset = be32(4) + be32(definition), size = be32(definition + 4);
            if ((magic & ~1u) != 0x102A1100 || offset + size > data.size() || size % 4)
            {
                Log("[test] %s: not a shader container", entry.path().filename().string().c_str());
                continue;
            }
            std::vector<uint32_t> ucode(size / 4);
            for (size_t i = 0; i < ucode.size(); i++)
                ucode[i] = be32(offset + i * 4);
            ShaderEntry shader;
            shader.info.stage = (magic & 1) ? ShaderStage::Vertex : ShaderStage::Pixel;
            shader.ucode = ucode;
            shader.info.hash = HashUcode(shader.info.stage, ucode.data(), uint32_t(ucode.size()));
            total++;
            bool ok = PrepareShader(&shader) != nullptr;
            failures += !ok;
            Log("[test] %-28s %s_%016llx %4u dwords, %3u instructions, %zu vfetch const(s), %zu texture(s)%s: %s",
                entry.path().filename().string().c_str(), (magic & 1) ? "vs" : "ps", (unsigned long long)shader.info.hash, uint32_t(ucode.size()), shader.info.instructionCount,
                shader.info.vertexFetches.size(), shader.info.textures.size(), shader.info.usesControlFlowMachine ? ", control flow" : "",
                ok ? "ok" : "FAILED");
        }
        Log("[test] %d shader(s), %d failed; HLSL in %s", total, failures, (g_shaderCacheDir.parent_path() / "shaderdump").string().c_str());
        return failures;
    }

    void* LoadShaderHandle(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount)
    {
        if (!g_active)
            return nullptr;
        uint64_t hash = HashUcode(stage, ucode, dwordCount);
        auto& slot = g_shaders[hash];
        if (!slot)
        {
            slot = std::make_unique<ShaderEntry>();
            slot->info.stage = stage;
            slot->info.hash = hash;
            slot->ucode.assign(ucode, ucode + dwordCount);
        }
        BindShader(stage, slot.get());
        return slot.get();  // entries live as long as the renderer (g_shaders never drops one)
    }

    void BindShader(ShaderStage stage, void* shader)
    {
        if (!g_active || !shader)
            return;
        if (stage == ShaderStage::Vertex)
            g_currentVertexShader = static_cast<ShaderEntry*>(shader);
        else
            g_currentPixelShader = static_cast<ShaderEntry*>(shader);
    }

    void LoadShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount)
    {
        LoadShaderHandle(stage, ucode, dwordCount);
    }

    void Draw(const DrawPacket& packet)
    {
        if (g_active)
            DrawImpl(packet);
    }

    void Swap(uint32_t frontBuffer, uint32_t width, uint32_t height, const uint32_t* fetchConstant)
    {
        if (!g_active)
            return;
        InvalidateDrawState();
        g_stats.swaps++;
        g_profileFrame = g_stats.swaps % 4 == 0;
        overlay::BeginFrame(g_stats.swaps);
        settings::Values options = settings::Get();
        g_effects.motionBlur = float(options.motionBlur) / 100.0f;
        g_effects.depthOfField = float(options.depthOfField) / 100.0f;
        g_effects.shadowSoftness = float(options.shadowSoftness) / 100.0f;
        // F8: the lighting remaster off / on (not saved), with a notice on screen.
        if (window::ConsumeLightingToggles() & 1)
        {
            bool on = lighting::ToggleBypass();
            bool any = on && (lighting::CurrentOptions().Any() || options.softShadows);
            overlay::Notice(!on ? "ILUMINACAO REMASTER: DESLIGADA (F8)"
                                : any ? "ILUMINACAO REMASTER: LIGADA (F8)" : "ILUMINACAO REMASTER: NADA ESCOLHIDO EM SETTINGS");
        }
        g_effects.jitter[2] = lighting::SoftShadows() ? 1.0f : 0.0f;
        g_lightingOptions = lighting::Ready() ? lighting::CurrentOptions() : lighting::Options{};
        if (g_anisotropy != options.anisotropy)
        {
            // Samplers are cached by their description, which now changes.
            g_anisotropy = options.anisotropy;
            Log("[gpu] anisotropic filtering %dx", g_anisotropy);
        }
        UpdateFullscreen();
        ResizeIfNeeded();

        ResolveTarget* source = FindResolveTarget(frontBuffer);
        if (!source && fetchConstant)
            source = FindResolveTarget(fetchConstant[1] & 0xFFFFF000);
        // CRASH_GPU_SHOW_RESOLVE=<physical address>: present that resolve target instead (debug).
        static const uint32_t s_showResolve = getenv("CRASH_GPU_SHOW_RESOLVE") ? uint32_t(strtoul(getenv("CRASH_GPU_SHOW_RESOLVE"), nullptr, 16)) : 0;
        bool showRaw = false;
        if (s_showResolve)
        {
            if (ResolveTarget* shown = FindResolveTarget(s_showResolve))
            {
                source = shown;
                showRaw = true;
            }
        }
        ResolveSurface* sourceSurface = source ? &source->Current() : nullptr;
        if (sourceSurface && !sourceSurface->texture.resource)
            source = nullptr, sourceSurface = nullptr;
        // Anti-aliasing at the front buffer's own size (internal resolution), before it is scaled
        // to the window. The targets are (re)created before anything of this present is recorded:
        // recreating them waits for the GPU, which submits and resets the command list (with an
        // upscaler on the front buffer changes size between the output and the scene scale).
        bool antiAliasing = false;
        if (source && !showRaw && options.antiAliasing != settings::AntiAliasing::Off)
        {
            uint32_t aaWidth = std::min(ScaledSize(width ? width : source->width, sourceSurface->scale), sourceSurface->hostWidth);
            uint32_t aaHeight = std::min(ScaledSize(height ? height : source->height, sourceSurface->scale), sourceSurface->hostHeight);
            antiAliasing = aaWidth && aaHeight && EnsureAaTargets(aaWidth, aaHeight);
        }

        uint32_t descriptor = AllocateDescriptors(1);
        uint32_t index = g_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = g_backBuffers[index].Get();
        TransitionRaw(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        BindDescriptorHeaps();
        g_list->SetGraphicsRootSignature(g_blitRootSignature.Get());
        // Keep the game's aspect ratio: the image is fitted into the window and centred, with
        // black bars at the sides or top and bottom, instead of being stretched to the window.
        float imageX = 0.0f, imageY = 0.0f, imageWidth = float(g_backBufferWidth), imageHeight = float(g_backBufferHeight);
        if (source && !showRaw)
        {
            float frameWidth = float(width ? width : source->width), frameHeight = float(height ? height : source->height);
            float aspect = frameWidth > 0.0f && frameHeight > 0.0f ? frameWidth / frameHeight : 16.0f / 9.0f;
            if (imageWidth > imageHeight * aspect + 0.5f)
                imageWidth = std::round(imageHeight * aspect);
            else if (imageHeight > imageWidth / aspect + 0.5f)
                imageHeight = std::round(imageWidth / aspect);
            imageX = std::floor((float(g_backBufferWidth) - imageWidth) * 0.5f);
            imageY = std::floor((float(g_backBufferHeight) - imageHeight) * 0.5f);
        }
        bool bars = imageWidth < float(g_backBufferWidth) || imageHeight < float(g_backBufferHeight);
        float constants[8] = { 1.0f, 1.0f, imageWidth, imageHeight, 0.0f, 0.0f, imageX, imageY };
        uint32_t flags = g_swapRedBlue ? 1u : 0u;
        // The front buffer is read like a texture, through the swizzle of the fetch constant
        // VdSwap was given (as Xenia's RequestSwapTexture does). D3D resolves the frame with
        // red/blue swapped (BGRA in memory, like D3DFMT_A8R8G8B8) and the fetch constant's
        // swizzle (ZYXW) swaps them back.
        uint32_t swizzle = 0x688; // XYZW
        if (fetchConstant)
        {
            xenos::TextureFetch fetch;
            memcpy(fetch.dword, fetchConstant, sizeof(fetch.dword));
            swizzle = fetch.Swizzle();
            static uint32_t s_loggedSwizzle = ~0u;
            if (s_loggedSwizzle != swizzle)
            {
                s_loggedSwizzle = swizzle;
                Log("[gpu] front buffer fetch constant: format %u, endian %u, swizzle 0x%03X", fetch.Format(), fetch.Endianness(), swizzle);
            }
        }
        memcpy(&constants[5], &swizzle, 4);
        if (source && showRaw)
        {
            bool packed = sourceSurface->packed.resource != nullptr;
            Transition(packed ? sourceSurface->packed : sourceSurface->texture, STATE_SHADER_RESOURCE);
            g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptor), CpuSrv(packed ? sourceSurface->packedSrvIndex : sourceSurface->srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            constants[0] = constants[1] = 1.0f;
            flags |= packed ? 4 : 8;
        }
        else if (source)
        {
            Transition(sourceSurface->texture, STATE_SHADER_RESOURCE);
            g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptor), CpuSrv(sourceSurface->srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            constants[0] = float(width ? width : source->width) / float(source->width);
            constants[1] = float(height ? height : source->height) / float(source->height);
            g_stats.presentedFrames++;
        }
        else
        {
            g_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptor), CpuSrv(g_nullTextureSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            flags |= 2;
        }
        memcpy(&constants[4], &flags, 4);
        g_list->SetGraphicsRootDescriptorTable(0, GpuHeapGpu(descriptor));
        g_list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        g_list->SetPipelineState(g_presentPipeline.Get());
        D3D12_VIEWPORT viewport{ imageX, imageY, imageWidth, imageHeight, 0.0f, 1.0f };
        D3D12_RECT scissor{ LONG(imageX), LONG(imageY), LONG(imageX + imageWidth), LONG(imageY + imageHeight) };
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (antiAliasing)
        {
            // 1:1 copy with the swizzle, then FXAA / SMAA, then the scaled present below.
            float copyConstants[8] = { constants[0], constants[1], float(g_aaWidth), float(g_aaHeight), constants[4], constants[5], 0.0f, 0.0f };
            g_list->SetGraphicsRoot32BitConstants(1, 8, copyConstants, 0);
            Transition(g_aaInput.texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_list->OMSetRenderTargets(1, &g_aaInput.rtv, FALSE, nullptr);
            D3D12_VIEWPORT aaViewport{ 0.0f, 0.0f, float(g_aaWidth), float(g_aaHeight), 0.0f, 1.0f };
            D3D12_RECT aaScissor{ 0, 0, LONG(g_aaWidth), LONG(g_aaHeight) };
            g_list->RSSetViewports(1, &aaViewport);
            g_list->RSSetScissorRects(1, &aaScissor);
            g_list->DrawInstanced(3, 1, 0, 0);
            Transition(g_aaInput.texture, STATE_SHADER_RESOURCE);
            Transition(g_aaOutput.texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (options.antiAliasing == settings::AntiAliasing::Smaa && smaa::Ready())
            {
                smaa::Apply(g_list.Get(), g_aaInput.texture.resource.Get(), g_aaOutput.rtv, g_aaWidth, g_aaHeight, g_stats.swaps);
                g_listNeedsHeaps = true;  // SMAA bound its own descriptor heap
                BindDescriptorHeaps();
                g_list->SetGraphicsRootSignature(g_blitRootSignature.Get());
            }
            else
            {
                uint32_t fxaaDescriptor = AllocateDescriptors(1);
                g_device->CopyDescriptorsSimple(1, GpuHeapCpu(fxaaDescriptor), CpuSrv(g_aaInput.srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                g_list->SetGraphicsRootDescriptorTable(0, GpuHeapGpu(fxaaDescriptor));
                g_list->SetPipelineState(g_fxaaPipeline.Get());
                g_list->OMSetRenderTargets(1, &g_aaOutput.rtv, FALSE, nullptr);
                g_list->DrawInstanced(3, 1, 0, 0);
            }
            Transition(g_aaOutput.texture, STATE_SHADER_RESOURCE);
            // The scaled present of the result: no swizzle, no red/blue swap.
            uint32_t outputDescriptor = AllocateDescriptors(1);
            g_device->CopyDescriptorsSimple(1, GpuHeapCpu(outputDescriptor), CpuSrv(g_aaOutput.srvIndex), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g_list->SetGraphicsRootDescriptorTable(0, GpuHeapGpu(outputDescriptor));
            float presentConstants[8] = { 1.0f, 1.0f, imageWidth, imageHeight, 0.0f, 0.0f, imageX, imageY };
            uint32_t identity = 0x688, noFlags = 0;
            memcpy(&presentConstants[4], &noFlags, 4);
            memcpy(&presentConstants[5], &identity, 4);
            g_list->SetGraphicsRoot32BitConstants(1, 8, presentConstants, 0);
            g_list->SetPipelineState(g_presentPipeline.Get());
        }
        g_list->OMSetRenderTargets(1, &g_backBufferRtvs[index], FALSE, nullptr);
        if (bars)
        {
            const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_list->ClearRenderTargetView(g_backBufferRtvs[index], black, 0, nullptr);
        }
        g_list->RSSetViewports(1, &viewport);
        g_list->RSSetScissorRects(1, &scissor);
        g_list->DrawInstanced(3, 1, 0, 0);

        // Subtitles (subtitles/subtitle_draw.h): part of the image, so screenshots have them.
        subtitles::Draw(g_list.Get(), g_backBufferRtvs[index], g_backBufferWidth, g_backBufferHeight);

        // The "hold to skip" prompt of the cutscenes and videos (skip_cinematics.h).
        cutscenes::Draw(g_list.Get(), g_backBufferRtvs[index], g_backBufferWidth, g_backBufferHeight);
        // The name entry's keyboard and the slot screen's DELETE (save_screens.h), then the settings
        // screen (settings_menu.h), drawn at the window's resolution.
        settings::save_screens::Draw(g_list.Get(), g_backBufferRtvs[index], g_backBufferWidth, g_backBufferHeight);
        settings::menu::Draw(g_list.Get(), g_backBufferRtvs[index], g_backBufferWidth, g_backBufferHeight);

        // Screenshots are taken before the overlay is drawn unless CRASH_SCREENSHOT_OVERLAY is set.
        uint64_t frame = g_stats.swaps;
        // input::ConsumeShotRequest: SHOT entries of a replay (CRASH_REPLAY).
        bool screenshot = window::ConsumeScreenshotRequest() || input::ConsumeShotRequest() ||
                          std::find(g_screenshotFrames.begin(), g_screenshotFrames.end(), frame) != g_screenshotFrames.end();
        for (auto it = g_screenshotAnchors.begin(); it != g_screenshotAnchors.end();)
        {
            int64_t at = boot::Resolve(*it);
            if (at < 0)
            {
                ++it;
                continue;
            }
            g_screenshotTimes.insert(std::upper_bound(g_screenshotTimes.begin(), g_screenshotTimes.end(), at), at);
            it = g_screenshotAnchors.erase(it);
        }
        if (!g_screenshotTimes.empty() && input::ClockMs() >= g_screenshotTimes.front())
        {
            while (!g_screenshotTimes.empty() && input::ClockMs() >= g_screenshotTimes.front())
                g_screenshotTimes.erase(g_screenshotTimes.begin());
            screenshot = true;
        }
        char screenshotName[64];
        snprintf(screenshotName, sizeof(screenshotName), "screenshot_%05llu.png", (unsigned long long)frame);
        bool overlayInShot = overlay::Visible() && overlay::InScreenshots();
        if (screenshot && !overlayInShot && overlay::Visible())
        {
            TransitionRaw(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            SaveScreenshot(backBuffer, g_screenshotDir / screenshotName);
            TransitionRaw(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            overlay::NoteScreenshot(frame);
            screenshot = false;
        }
        overlay::Draw(g_list.Get(), g_backBufferRtvs[index], g_backBufferWidth, g_backBufferHeight);
        TransitionRaw(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (screenshot)
        {
            SaveScreenshot(backBuffer, g_screenshotDir / screenshotName);
            overlay::NoteScreenshot(frame);
        }

        LARGE_INTEGER submitStart, submitEnd, presentEnd;
        QueryPerformanceCounter(&submitStart);
        Submit();
        QueryPerformanceCounter(&submitEnd);
        // ALLOW_TEARING is invalid in exclusive fullscreen (there DXGI already presents unthrottled).
        BOOL exclusive = FALSE;
        if (window::Mode() == settings::WindowMode::Fullscreen)
            g_swapChain->GetFullscreenState(&exclusive, nullptr);
        g_swapChain->Present(options.vsync ? 1 : 0, !options.vsync && g_allowTearing && !exclusive ? DXGI_PRESENT_ALLOW_TEARING : 0);
        QueryPerformanceCounter(&presentEnd);
        g_submitTicks += uint64_t(submitEnd.QuadPart - submitStart.QuadPart);
        g_presentTicks = std::max(g_presentTicks, uint64_t(presentEnd.QuadPart - submitEnd.QuadPart));

        // Frame pacing stats: host time between presents, logged every 300 frames.
        {
            static LARGE_INTEGER s_frequency, s_last;
            static double s_sum, s_max;
            static uint32_t s_count, s_hitches;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (!s_frequency.QuadPart)
                QueryPerformanceFrequency(&s_frequency);
            if (s_last.QuadPart)
            {
                double ms = double(now.QuadPart - s_last.QuadPart) * 1000.0 / double(s_frequency.QuadPart);
                s_sum += ms;
                s_max = std::max(s_max, ms);
                s_count++;
                if (ms > 50.0)
                    s_hitches++;
                if (s_count == 300)
                {
                    Log("[gpu] present pacing: %u frames, avg %.2f ms, max %.2f ms, %u over 50 ms; waiting for the GPU %.2f ms/frame, submit %.2f ms/frame, longest Present %.2f ms, ring waits %.2f ms/frame; window %s",
                        s_count, s_sum / s_count, s_max, s_hitches, double(g_frameWaitTicks) * 1000.0 / double(s_frequency.QuadPart) / s_count,
                        double(g_submitTicks) * 1000.0 / double(s_frequency.QuadPart) / s_count, double(g_presentTicks) * 1000.0 / double(s_frequency.QuadPart),
                        double(g_ringWaitTicks) * 1000.0 / double(s_frequency.QuadPart) / s_count,
                        GetForegroundWindow() == g_hwnd ? "focused" : "not focused");
                    g_submitTicks = g_presentTicks = g_ringWaitTicks = 0;
                    g_frameWaitTicks = 0;
                    std::string profile;
                    for (int i = 0; i < 10; i++)
                    {
                        char text[64];
                        snprintf(text, sizeof(text), "%s%s %.2f", i ? ", " : "", PROFILE_NAMES[i], double(g_profileTicks[i]) * 4.0 * 1000.0 / double(s_frequency.QuadPart) / 300.0);
                        profile += text;
                        g_profileTicks[i] = 0;
                    }
                    Log("[gpu] CP thread ms/frame: %s", profile.c_str());
                    static uint64_t s_faults;
                    uint64_t faults = watch::Faults();
                    Log("[gpu] CP work/frame: %.0f draws, %.0f indices, %.0f texture lookups, %.0f textures hashed (%.2f MB), %.0f unchanged by write watch, %.1f protected, %.1f write faults, %.1f texture uploads, %.2f MB of PM4",
                        g_work.draws / 300.0, g_work.indices / 300.0, g_work.textureLookups / 300.0, g_work.textureChecks / 300.0,
                        g_work.textureBytes / 300.0 / 1048576.0, g_work.textureWatched / 300.0, g_work.textureProtects / 300.0,
                        double(faults - s_faults) / 300.0, g_work.textureUploads / 300.0, double(gpu::TakeCommandDwords()) * 4.0 / 300.0 / 1048576.0);
                    s_faults = faults;
                    g_work = {};
                    s_sum = s_max = 0.0;
                    s_count = s_hitches = 0;
                }
            }
            s_last = now;
        }
        g_frameFences.push_back(g_fenceValue);
        LARGE_INTEGER waitStart, waitEnd;
        QueryPerformanceCounter(&waitStart);
        while (g_frameFences.size() > 2)
        {
            WaitForFence(g_frameFences.front());
            g_frameFences.pop_front();
        }
        QueryPerformanceCounter(&waitEnd);
        g_frameWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
        g_frame++;
        if (g_reconfigureRequested.exchange(false))
            Reconfigure();
        NextTemporalFrame(g_frame);

        if (frame <= 3 || frame % 300 == 0)
            Log("[gpu] presented frame %llu: front buffer 0x%08X %ux%u %s", (unsigned long long)frame, frontBuffer, width, height,
                source ? "(resolved)" : "(no resolve target)");

        // EXIT GAME (settings/quit_game.h), finished on the thread that owns the device.
        if (settings::QuitRequested())
        {
            WaitIdle();
            BOOL fullscreen = FALSE;
            if (SUCCEEDED(g_swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen)
                g_swapChain->SetFullscreenState(FALSE, nullptr);
            upscaler::Shutdown();
            Log("[gpu] exit: GPU idle, windowed, upscalers released");
            settings::FinishQuit();
        }
    }

    void RequestReconfigure()
    {
        g_reconfigureRequested = true;
    }

    void LogStats()
    {
        if (!g_active)
            return;
        Log("[gpu] renderer: %llu draws (%llu skipped), %llu resolves, %llu clears (%llu depth-alias), %llu swaps (%llu with a resolved front buffer), %llu texture uploads, "
            "%zu textures (%llu hd), %zu render targets, %zu resolve targets, %zu shaders (%llu compiled, %llu failed), %llu pipelines, %llu submits",
            (unsigned long long)g_stats.draws, (unsigned long long)g_stats.drawsSkipped, (unsigned long long)g_stats.resolves,
            (unsigned long long)g_stats.clears, (unsigned long long)g_stats.aliasClears, (unsigned long long)g_stats.swaps, (unsigned long long)g_stats.presentedFrames,
            (unsigned long long)g_stats.textureUploads, g_textures.size(), (unsigned long long)g_stats.hdTextures, g_renderTargets.size(), g_resolveTargets.size(), g_shaders.size(),
            (unsigned long long)g_stats.shaderCompiles, (unsigned long long)g_stats.shaderFailures, (unsigned long long)g_stats.pipelines,
            (unsigned long long)g_stats.submits);
        std::string reasons;
        for (uint32_t i = 0; i < std::size(SKIP_NAMES); i++)
        {
            if (g_stats.skipReasons[i])
                reasons += std::string(reasons.empty() ? "" : ", ") + SKIP_NAMES[i] + " " + std::to_string(g_stats.skipReasons[i]);
        }
        if (!reasons.empty())
            Log("[gpu] skipped draws: %s", reasons.c_str());
        if (g_temporal.active)
        {
            static uint64_t s_lastEvaluations;
            uint64_t frames = g_temporal.evaluations - s_lastEvaluations;
            Log("[gpu] temporal upscaling (%s): %llu frames upscaled, %llu failed; %.2f ms of CPU per upscale", upscaler::ActiveName().c_str(),
                (unsigned long long)g_temporal.evaluations, (unsigned long long)g_temporal.failures, frames ? g_temporal.evaluateMs / double(frames) : 0.0);
            s_lastEvaluations = g_temporal.evaluations;
            g_temporal.evaluateMs = 0.0;
        }
        uint32_t lightingFrames;
        double lightingMs = lighting::TakeGpuMs(lightingFrames);
        if (lightingFrames)
            Log("[lighting] %u frames, %.3f ms of GPU per frame (ssao %s, gi %s, contact shadows %s, scene %ux%u)", lightingFrames, lightingMs,
                g_lightingOptions.ao ? (g_lightingOptions.fullResolution ? "high" : "low") : "off", g_lightingOptions.gi ? "on" : "off",
                g_lightingOptions.contactShadows ? "on" : "off", g_temporal.renderWidth, g_temporal.renderHeight);
    }
}
