// Xenos renderer: turns the Xenos register state at each draw into work for the graphics API
// backend (rhi.h: Direct3D 12 or Vulkan). Everything here is API-agnostic: EDRAM render targets,
// resolves, texture decoding and caching, the temporal upscaling inputs, presentation.
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
//   are done with the backend's ClearColor/ClearDepthStencil.
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
#include "rhi.h"
#include "shader_warmup.h"
#include "smaa.h"
#include "upscaler.h"
#include "texture_watch.h"
#include "graphics_options.h"
#include "hd_textures.h"
#include "lighting.h"
#include "overlay.h"
#include "png_writer.h"
#include "texture_decode.h"
#include "widescreen.h"
#include "window.h"
#include "xenos.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../kernel/input.h"
#include "../kernel/memory.h"
#include "../paths.h"
#include "../settings/challenge_counters.h"
#include "../settings/direct_boot.h"
#include "../settings/prompt_icons.h"
#include "../settings/quit_game.h"
#include "../settings/options.h"
#include "../settings/settings.h"
#include "../settings/save_screens.h"
#include "../settings/menu_mouse.h"
#include "../settings/skip_cinematics.h"
#include "../settings/touch_controls.h"
#include "../settings/settings_menu.h"
#include "../settings/ui_text.h"
#include "../dev/dev_console.h"
#include "../dev/cheat_menu.h"
#include "../achievements/achievements.h"
#include "../online/mp_menu.h"
#include "../subtitles/subtitle_draw.h"
#include "../platform/platform.h"
#include "../platform/text.h"
#include "../platform/crt.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#else
#include <emmintrin.h>
#endif

namespace gpu::renderer
{
    namespace
    {
        using rhi::HashBytes;

        constexpr uint32_t WINDOW_WIDTH = 1280;
        constexpr uint32_t WINDOW_HEIGHT = 720;
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
        // Wide screens (widescreen.h): host x per guest x is the scale times this (Aspect / 16:9);
        // the scales above are per guest y. 1 at 16:9.
        float g_stretch = 1.0f;
        float XScale(float scale) { return scale * g_stretch; }

        uint32_t ScaledSize(uint32_t value, float scale) { return uint32_t(ceilf(float(value) * scale)); }
        int32_t ScaledFloor(float value, float scale) { return int32_t(floorf(value * scale + 0.001f)); }
        int32_t ScaledCeil(float value, float scale) { return int32_t(ceilf(value * scale - 0.001f)); }
        constexpr uint32_t MAX_VERTEX_RANGE_BYTES = 32u << 20;

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
            float renderScale[4];      // xy: guest pixels per host pixel
            float texSizeScale[64];    // xy per fetch constant: guest texels per host texel
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

        // Content hash of guest textures, checked once per frame for every texture a frame uses
        // (~25 MB per frame in the village). XXH3's accumulation on SSE2 lanes: per 16 bytes, a
        // 32x32->64 multiply of the input (xor a key) by its own swapped halves, plus the input,
        // into 64-bit accumulators. ~2.5x the speed of HashBytes, whose 64-bit multiply chain
        // limited it to ~7.5 GB/s (tools/perf, runtime/NOTES.md "Desempenho"). Only compared with
        // itself (TextureEntry::contentHash), so it never has to match HashBytes.
        uint64_t HashContent(const void* data, size_t size, uint64_t seed = 0)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            alignas(16) uint64_t lanes[8];
            size_t i = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
            // The same lanes with NEON: vmull_u32 of each 64-bit lane's low and high halves is
            // _mm_mul_epu32 against the swapped halves; vextq_u64 swaps the two 64-bit lanes.
            uint64x2_t acc[4] = { vdupq_n_u64(seed ^ 0x243F6A8885A308D3ull), vdupq_n_u64(seed ^ 0x13198A2E03707344ull),
                                  vdupq_n_u64(seed ^ 0xA4093822299F31D0ull), vdupq_n_u64(seed ^ 0x082EFA98EC4E6C89ull) };
            const uint64x2_t key[4] = { vcombine_u64(vcreate_u64(0xBE4BA423396CFEB8ull), vcreate_u64(0x7C01812CF721AD1Cull)),
                                        vcombine_u64(vcreate_u64(0x1CAD21F72C81017Cull), vcreate_u64(0xDED46DE9839097DBull)),
                                        vcombine_u64(vcreate_u64(0x2172FFCC7DD05A82ull), vcreate_u64(0x78E5C0CC4EE679CBull)),
                                        vcombine_u64(vcreate_u64(0x4C263A81E69035E0ull), vcreate_u64(0x8E2443F7744608B8ull)) };
            for (; i + 64 <= size; i += 64)
            {
                for (int k = 0; k < 4; k++)
                {
                    uint64x2_t v = vreinterpretq_u64_u8(vld1q_u8(bytes + i + 16 * k));
                    uint64x2_t x = veorq_u64(v, key[k]);
                    uint64x2_t product = vmull_u32(vmovn_u64(x), vshrn_n_u64(x, 32));
                    acc[k] = vaddq_u64(acc[k], vextq_u64(v, v, 1));
                    acc[k] = vaddq_u64(acc[k], product);
                }
            }
            for (int k = 0; k < 4; k++)
                vst1q_u64(lanes + 2 * k, acc[k]);
#else
            __m128i acc[4] = { _mm_set1_epi64x(int64_t(seed ^ 0x243F6A8885A308D3ull)), _mm_set1_epi64x(int64_t(seed ^ 0x13198A2E03707344ull)),
                               _mm_set1_epi64x(int64_t(seed ^ 0xA4093822299F31D0ull)), _mm_set1_epi64x(int64_t(seed ^ 0x082EFA98EC4E6C89ull)) };
            const __m128i key[4] = { _mm_set_epi64x(int64_t(0x7C01812CF721AD1Cull), int64_t(0xBE4BA423396CFEB8ull)),
                                     _mm_set_epi64x(int64_t(0xDED46DE9839097DBull), int64_t(0x1CAD21F72C81017Cull)),
                                     _mm_set_epi64x(int64_t(0x78E5C0CC4EE679CBull), int64_t(0x2172FFCC7DD05A82ull)),
                                     _mm_set_epi64x(int64_t(0x8E2443F7744608B8ull), int64_t(0x4C263A81E69035E0ull)) };
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
            for (int k = 0; k < 4; k++)
                _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 2 * k), acc[k]);
#endif
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

        struct ShaderEntry
        {
            TranslatedShader info;
            std::vector<uint32_t> ucode;
            rhi::Shader* shader = nullptr;  // owned by the backend
            bool translated = false;
            bool failed = false;
        };

        struct RenderTarget
        {
            rhi::Texture* texture = nullptr;
            DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;  // render view format, or the depth format
            DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
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
            rhi::Texture* texture = nullptr;
            uint32_t hostWidth = 0, hostHeight = 0;  // the resource (scaled)
            float scale = 0.0f;                      // 0: not created

            // Depth resolves only: the 32-bit words the console writes to memory, (depth24 << 8) |
            // stencil, as RGBA8 bytes (R = stencil). Fetches with a non-depth format (the game reads
            // the stencil of a depth resolve as k_8_8_8_8 for its stencil shadows) sample this one;
            // k_24_8/k_24_8_FLOAT fetches sample the float depth in `texture`.
            rhi::Texture* packed = nullptr;
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
            rhi::Texture* texture = nullptr;
            uint32_t width = 0, height = 0;
            bool failed = false;
        };

        struct TextureEntry
        {
            rhi::Texture* texture = nullptr;
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
            uint64_t hudDraws = 0;         // 2D of the game's vertex shader on a wide screen
            uint64_t hudFolded = 0;        // of those, done in c4..c7 (HudFoldDraw) instead of HUD_GS
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
            SKIP_COMPILING,
        };
        const char* const SKIP_NAMES[] = { "edram mode", "viz query", "shader", "primitive", "no target", "pipeline", "culled", "compiling" };
        uint32_t g_traceDraws = 0;
        uint64_t g_traceFrom = 0;
        uint64_t g_traceSwap = UINT64_MAX; // CRASH_GPU_TRACE_SWAP=N: every draw and resolve of the frame presented by swap N

        // CRASH_GPU_TRACE_SWAPS=N: that many frames from CRASH_GPU_TRACE_SWAP (default 1).
        const uint64_t g_traceSwapCount = getenv("CRASH_GPU_TRACE_SWAPS") ? strtoull(getenv("CRASH_GPU_TRACE_SWAPS"), nullptr, 10) : 1;

        rhi::ShaderCompilation RhiShaderCompilation(settings::ShaderCompilation mode)
        {
            return mode == settings::ShaderCompilation::Sync         ? rhi::ShaderCompilation::Sync
                   : mode == settings::ShaderCompilation::Aggressive ? rhi::ShaderCompilation::Aggressive
                                                                     : rhi::ShaderCompilation::Async;
        }

        bool TracingSwap()
        {
            return g_stats.swaps + 1 >= g_traceSwap && g_stats.swaps + 1 < g_traceSwap + g_traceSwapCount;
        }

        // CRASH_GPU_PROBE=<first>-<last> (debug): the color resolves and presents of those swaps are
        // sampled on the GPU and logged when it gets there ([probe] lines), without waiting for it.
        bool ProbingSwap(uint64_t swap)
        {
            static const std::pair<uint64_t, uint64_t> s_range = [] {
                const char* text = getenv("CRASH_GPU_PROBE");
                if (!text)
                    return std::pair<uint64_t, uint64_t>(1, 0);
                char* end;
                uint64_t first = strtoull(text, &end, 10);
                return std::pair<uint64_t, uint64_t>(first, *end == '-' ? strtoull(end + 1, nullptr, 10) : first);
            }();
            return swap >= s_range.first && swap <= s_range.second;
        }

        void Skip(SkipReason reason)
        {
            g_stats.drawsSkipped++;
            g_stats.skipReasons[reason]++;
        }

        // Device: the graphics API backend (rhi.h) and the window.
        std::atomic<bool> g_active;
        rhi::Device* g_rhi = nullptr;

        // CRASH_GPU_PROFILE: what the GPU work recorded from here on is for (the label of its rows).
        __attribute__((format(printf, 1, 2))) void ProfileLabel(const char* format, ...)
        {
            if (!g_rhi->Profiling())
                return;
            char label[96];
            va_list args;
            va_start(args, format);
            vsnprintf(label, sizeof(label), format, args);
            va_end(args);
            g_rhi->ProfileLabel(label);
        }

        bool g_debugLayer = false;
        void* g_window;
        uint32_t g_backBufferWidth = WINDOW_WIDTH, g_backBufferHeight = WINDOW_HEIGHT;

        // Pipelines and shaders.
        std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> g_shaders;
        ShaderEntry* g_currentVertexShader;
        ShaderEntry* g_currentPixelShader;
        // Graphics options of the current frame (from the settings, updated in Swap).
        graphics::EffectConstants g_effects;
        int g_anisotropy = 1;
        rhi::Shader* g_rectListGs = nullptr;
        rhi::Shader* g_hudGs = nullptr;  // HUD_GS (wide screens)
        rhi::Shader* g_depthAliasPs = nullptr;
        std::unordered_map<uint32_t, rhi::Pipeline*> g_resolvePipelines;  // by DXGI format
        rhi::Pipeline* g_resolveDepthPackedPipeline = nullptr;
        std::unordered_map<uint32_t, rhi::Pipeline*> g_presentPipelines;  // by target format
        // Post-processing anti-aliasing (settings: video.antialiasing), at the size of the front
        // buffer: the present pass first draws it 1:1 into g_aaInput (applying the fetch constant
        // swizzle), FXAA or SMAA write g_aaOutput, and a last present pass scales that to the back
        // buffer.
        rhi::Pipeline* g_fxaaPipeline = nullptr;
        rhi::Texture* g_aaInput = nullptr;
        rhi::Texture* g_aaOutput = nullptr;
        uint32_t g_aaWidth = 0, g_aaHeight = 0;
        std::filesystem::path g_shaderCacheDir;
        bool g_dumpShaders = false;

        // Caches.
        std::unordered_map<uint64_t, RenderTarget> g_renderTargets;
        std::unordered_map<uint32_t, ResolveTarget> g_resolveTargets; // by physical base address
        std::unordered_map<uint64_t, TextureEntry> g_textures;
        std::unordered_map<uint64_t, HdTexture> g_hdTextures;  // by key (hd_textures.h)
        // Button icons drawn for the device in use (settings/prompt_icons.h), by icon key and style.
        std::unordered_map<uint64_t, IconTexture> g_iconTextures;
        // The palette of the icon bound by the draw being set up (GetTextureView), which replaces the
        // draw's 256x1 palette texture; null = none.
        rhi::Texture* g_drawIconPalette = nullptr;
        char32_t g_drawIcon = 0;  // the button icon the draw samples (prompt_icons.h), 0 none
        std::vector<settings::prompts::IconSpot> g_iconSpots;  // this frame's (clickable prompts)
        bool g_pageNoted = false;  // this frame drew the frontend's page through its camera (NotePage)
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
        bool g_swapRedBlue = false;

        void WaitIdle()
        {
            g_rhi->WaitIdle();
        }

        // Upload memory (the backend's ring): cpu = null if it doesn't fit.
        rhi::Device::Upload AllocateUpload(uint64_t size, uint64_t alignment)
        {
            return g_rhi->AllocateUpload(size, alignment);
        }

        rhi::Texture* CreateTexture(uint32_t width, uint32_t height, uint32_t levels, DXGI_FORMAT format, uint32_t usage, const char* name,
                                    DXGI_FORMAT renderFormat = DXGI_FORMAT_UNKNOWN)
        {
            rhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.levels = levels;
            desc.format = format;
            desc.renderFormat = renderFormat;
            desc.usage = usage;
            desc.name = name;
            return g_rhi->CreateTexture(desc);
        }

        void DestroyTexture(rhi::Texture*& texture)
        {
            if (texture)
                g_rhi->DestroyTexture(texture);
            texture = nullptr;
        }

        // Copies decoded levels written at `offset` in the upload ring (consecutive LevelFootprint
        // layouts, each aligned to UPLOAD_PLACEMENT_ALIGNMENT) into `texture`.
        uint64_t UploadLayout(DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t levels, rhi::Footprint* footprints, uint64_t* offsets)
        {
            uint64_t total = 0;
            for (uint32_t level = 0; level < levels; level++)
            {
                footprints[level] = rhi::LevelFootprint(format, width, height, level);
                offsets[level] = total;
                total = AlignUp64(total + footprints[level].size, rhi::UPLOAD_PLACEMENT_ALIGNMENT);
            }
            return total;
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
                    // The microcode as in guest memory (big-endian dwords): tools/shaders.py finds
                    // where the game keeps it.
                    std::vector<uint32_t> bigEndian(entry->ucode.size());
                    for (size_t i = 0; i < bigEndian.size(); i++)
                        bigEndian[i] = __builtin_bswap32(entry->ucode[i]);
                    std::ofstream(dumpPath / (std::string(name) + ".ucode"), std::ios::binary)
                        .write(reinterpret_cast<const char*>(bigEndian.data()), std::streamsize(bigEndian.size() * 4));
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
                rhi::Stage stage = entry->info.stage == ShaderStage::Pixel ? rhi::Stage::Pixel : rhi::Stage::Vertex;
                if (g_rhi)
                {
                    entry->shader = g_rhi->CreateShader(stage, entry->info.hlsl, name, rhi::Layout::Draw);
                    entry->failed = !entry->shader;
                }
                else
                {
                    // --test-shaders: compiled only.
                    entry->failed = !rhi::TestCompile(stage, entry->info.hlsl, name, rhi::Layout::Draw, g_shaderCacheDir);
                }
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

// Rectangle list: 3 vertices per rectangle; the 4th corner is opposite the right angle. Constant
// indices into `input` only: Mali's compiler (G720, r44p1) crashes on a GS input indexed at run time.
[maxvertexcount(4)]
void main(triangle XeVertexOutput input[3], inout TriangleStream<XeVertexOutput> stream)
{
    float2 p0 = input[0].position.xy / input[0].position.w;
    float2 p1 = input[1].position.xy / input[1].position.w;
    float2 p2 = input[2].position.xy / input[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    if (d12 >= d01 && d12 >= d20)
    {
        stream.Append(input[0]);
        stream.Append(input[1]);
        stream.Append(input[2]);
        stream.Append(Combine(input[1], input[2], input[0]));
    }
    else if (d20 >= d01)
    {
        stream.Append(input[1]);
        stream.Append(input[2]);
        stream.Append(input[0]);
        stream.Append(Combine(input[2], input[0], input[1]));
    }
    else
    {
        stream.Append(input[2]);
        stream.Append(input[0]);
        stream.Append(input[1]);
        stream.Append(Combine(input[0], input[1], input[2]));
    }
}
)";

        // Wide screens: the 2D triangles of the screen back into the middle 16:9 area (see
        // HudTransform). Per triangle, so an element touching an edge moves in whole: only a
        // triangle that spans the screen's width (fades, full-screen passes, backgrounds, bars)
        // keeps its x at the edges, and a background (one that spans both ways, or a big one past
        // an edge: the frontend's background sprites) zooms to fill the screen.
        // xe_hud as in shader_translator.cpp (mode 2: fill, 4: just scaled, 8: anchored corners).
        const char* HUD_GS = R"(
cbuffer xe_effects : register(b3)
{
    float4 xe_effect;
    float4 xe_jitter;
    float4 xe_hud;
    float4 xe_hud_target;
};

struct XeVertexOutput
{
    float4 interpolators[16] : TEXCOORD0;
    float4 position : SV_Position;
};

[maxvertexcount(3)]
void main(triangle XeVertexOutput input[3], inout TriangleStream<XeVertexOutput> stream)
{
    uint mode = uint(xe_hud.z + 0.5);
    // W = 1: the game's own 2D, laid out for the whole 16:9 screen. Another W, the same on the three
    // vertices: 2D drawn through a pure3d camera (the frontend's sprites, 640x480 at W = 0.5), which
    // camera/hor_plus.cpp already put into the middle 16:9 area.
    float w = input[0].position.w;
    bool flat = w > 0.0 && abs(input[1].position.w - w) <= 1e-5 * w && abs(input[2].position.w - w) <= 1e-5 * w;
    bool projected = flat && abs(w - 1.0) > 1e-5;
    float2 px[3];
    [unroll] for (uint i = 0; i < 3u; i++)
    {
        float2 ndc = input[i].position.xy / w;
        px[i] = float2((ndc.x + 1.0) * 0.5 * xe_hud_target.x, (1.0 - ndc.y) * 0.5 * xe_hud_target.y);
    }
    float2 centre = xe_hud_target.zw * 0.5;
    // The area the layout covers: the screen, or its middle 16:9 area for projected 2D.
    float2 half = projected ? centre * min(xe_hud.xy, 1.0) : centre;
    float2 lo = min(px[0], min(px[1], px[2])), hi = max(px[0], max(px[1], px[2]));
    bool2 full = lo <= centre - half + 2.0 && hi >= centre + half - 2.0;
    // A background: a triangle that spans the area both ways, or one big both ways that goes past
    // its edges (the frontend's backgrounds are large sprites hanging out of the screen; the bars
    // of button prompts are wide but low).
    float2 extent = hi - lo;
    bool past = any(lo < centre - half - 2.0) || any(hi > centre + half + 2.0);
    bool background = (full.x && full.y) || (past && extent.x > half.x * 0.5 && extent.y > half.y * 0.5);

    [unroll] for (uint j = 0; j < 3u; j++)
    {
        XeVertexOutput v = input[j];
        if (flat)
        {
            float2 d = px[j] - centre;
            float2 result = px[j];
            if (projected)
            {
                // Already in place; a background zooms (keeping its aspect) to fill the screen.
                if ((mode & 2u) && !(mode & 4u) && background) result = centre + d / min(xe_hud.x, xe_hud.y);
            }
            else
            {
                result = centre + d * xe_hud.xy;
                if (!(mode & 4u))
                {
                    if ((mode & 8u) && !full.x)
                        result.x += xe_hud.w;  // anchored HUD: the draw's side (signed), HudDrawSide
                    if (full.x) result.x = px[j].x;
                    if (full.y) result.y = px[j].y;
                    if ((mode & 2u) && full.x && full.y) result = centre + d * float2(1.0 / xe_hud.y, 1.0 / xe_hud.x);
                }
            }
            v.position.xy = float2(result.x / xe_hud_target.x * 2.0 - 1.0, 1.0 - result.y / xe_hud_target.y * 2.0) * w;
        }
        stream.Append(v);
    }
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
#ifdef __spirv__
#define XE_STENCIL x
#else
#define XE_STENCIL y
#endif
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
    uint s = stencil.Load(int3(p, 0)).XE_STENCIL & 0xFFu;
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

        constexpr size_t PRESENT_CONSTANTS = 12;  // PRESENT_PS's cbuffer, in floats (with its padding)
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
    float gamma;     // [video] gamma / 100: above 1 brighter (1 = unchanged)
    float saturation;  // [video] saturation / 100 (1 = unchanged)
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
    if (saturation != 1.0)
        v = saturate(lerp(dot(v, float3(0.299, 0.587, 0.114)).xxx, v, saturation));
    if (gamma != 1.0)
        v = pow(saturate(v), 1.0 / gamma);
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

        // The texture of `target` at its scale (g_sceneScale for scene targets, g_renderScale for
        // the others), cleared; recreated at another scale by Reconfigure.
        void CreateRenderTargetResource(RenderTarget& target)
        {
            bool isDepth = target.isDepth;
            uint32_t format = target.guestFormat;
            DestroyTexture(target.texture);
            target.scale = target.IsScene() ? g_sceneScale : g_renderScale;
            target.hostWidth = ScaledSize(target.width, XScale(target.scale));
            target.hostHeight = ScaledSize(target.height, target.scale);

            if (isDepth)
            {
                bool isFloat = format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
                target.rtvFormat = isFloat ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
                target.srvFormat = target.rtvFormat;
                target.texture = CreateTexture(target.hostWidth, target.hostHeight, 1, target.rtvFormat, rhi::USAGE_SAMPLED | rhi::USAGE_DEPTH_STENCIL,
                                               "depth render target");
            }
            else
            {
                target.rtvFormat = ColorRtvFormat(format);
                target.srvFormat = ColorSrvFormat(target.rtvFormat);
                target.texture = CreateTexture(target.hostWidth, target.hostHeight, 1, target.srvFormat, rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET,
                                               "color render target", target.rtvFormat);
            }
            if (!target.texture)
                Fatal("[gpu] could not create a %ux%u render target", target.hostWidth, target.hostHeight);
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

            for (ResolveSurface& surface : target.surfaces)
            {
                DestroyTexture(surface.texture);
                DestroyTexture(surface.packed);
            }
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

        // The surface of `target` at `scale`, created (cleared) on first use.
        ResolveSurface& EnsureSurface(ResolveTarget& target, float scale)
        {
            ResolveSurface& surface = target.surfaces[SurfaceIndex(scale)];
            if (surface.texture)
                return surface;
            surface.scale = scale;
            surface.hostWidth = ScaledSize(target.width, XScale(scale));
            surface.hostHeight = ScaledSize(target.height, scale);
            surface.texture = CreateTexture(surface.hostWidth, surface.hostHeight, 1, target.format, rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET,
                                            "resolve target");
            if (!surface.texture)
                Fatal("[gpu] could not create a %ux%u resolve target", surface.hostWidth, surface.hostHeight);
            return surface;
        }

        // Creates the packed depth/stencil companion of a depth resolve surface on first use.
        void EnsurePackedDepth(ResolveSurface& target)
        {
            if (target.packed)
                return;
            target.packed = CreateTexture(target.hostWidth, target.hostHeight, 1, DXGI_FORMAT_R8G8B8A8_UNORM, rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET,
                                          "packed depth resolve target");
            if (!target.packed)
                Fatal("[gpu] could not create a %ux%u packed depth resolve target", target.hostWidth, target.hostHeight);
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
            int64_t t0, t1, t2, t3, frequency;
            frequency = int64_t(plat::PerfFrequency());
            t0 = int64_t(plat::PerfCounter());
            hd::Image image;
            if (!hd::Load(key, srgb, image))
                return nullptr;
            t1 = int64_t(plat::PerfCounter());
            if (image.width < guestWidth || image.height < guestHeight ||
                uint64_t(image.width) * guestHeight != uint64_t(image.height) * guestWidth)
            {
                Log("[gpu] hd texture %016llX: %ux%u is not an enlargement of the guest's %ux%u with the same aspect (%s); not used",
                    (unsigned long long)key, image.width, image.height, guestWidth, guestHeight, image.source.c_str());
                return nullptr;
            }
            hdTexture.texture = CreateTexture(image.width, image.height, image.levels, image.format, rhi::USAGE_SAMPLED, "hd texture");
            if (!hdTexture.texture)
            {
                Log("[gpu] hd texture %016llX: could not create a %ux%u texture (%s)", (unsigned long long)key, image.width, image.height,
                    image.source.c_str());
                return nullptr;
            }
            rhi::Footprint footprints[16];
            uint64_t levelOffsets[16];
            uint32_t levels = std::min(image.levels, 16u);
            uint64_t totalBytes = UploadLayout(image.format, image.width, image.height, levels, footprints, levelOffsets);
            t2 = int64_t(plat::PerfCounter());
            rhi::Device::Upload upload = AllocateUpload(totalBytes, rhi::UPLOAD_PLACEMENT_ALIGNMENT);
            t3 = int64_t(plat::PerfCounter());
            if (!upload.cpu)
            {
                Log("[gpu] hd texture %016llX: %llu bytes don't fit the upload ring (%s)", (unsigned long long)key,
                    (unsigned long long)totalBytes, image.source.c_str());
                DestroyTexture(hdTexture.texture);
                return nullptr;
            }
            for (uint32_t level = 0; level < levels; level++)
            {
                const hd::Image::Level& source = image.layout[level];
                for (uint32_t row = 0; row < source.rows && row < footprints[level].rows; row++)
                    memcpy(upload.cpu + levelOffsets[level] + uint64_t(row) * footprints[level].rowPitch,
                           image.data.data() + source.offset + size_t(row) * source.rowBytes, source.rowBytes);
                g_rhi->CopyUploadToTexture(hdTexture.texture, level, upload.offset + levelOffsets[level], footprints[level].rowPitch);
            }
            hdTexture.width = image.width;
            hdTexture.height = image.height;
            hdTexture.failed = false;
            g_stats.hdTextures++;
            int64_t t4 = int64_t(plat::PerfCounter());
            auto ms = [&](int64_t a, int64_t b) { return double(b - a) * 1000.0 / double(frequency); };
            Log("[gpu] hd texture %016llX: %ux%u -> %ux%u, %u mip(s), DXGI %u (%s); %.1f ms (load %.1f, create %.1f, upload wait %.1f, copy %.1f)",
                (unsigned long long)key, guestWidth, guestHeight, image.width, image.height, image.levels, uint32_t(image.format), image.source.c_str(),
                ms(t0, t4), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
            return &hdTexture;
        }

        // Creates a 1-level texture from tightly packed rows (bottom row first, as the guest's).
        bool UploadSmallTexture(HdTexture& texture, DXGI_FORMAT format, uint32_t w, uint32_t h, const uint8_t* data, uint32_t rowBytes)
        {
            texture.texture = CreateTexture(w, h, 1, format, rhi::USAGE_SAMPLED, "icon texture");
            if (!texture.texture)
                return false;
            rhi::Footprint footprint = rhi::LevelFootprint(format, w, h, 0);
            rhi::Device::Upload upload = AllocateUpload(footprint.size, rhi::UPLOAD_PLACEMENT_ALIGNMENT);
            if (!upload.cpu)
            {
                DestroyTexture(texture.texture);
                return false;
            }
            for (uint32_t y = 0; y < h; y++)
                memcpy(upload.cpu + uint64_t(y) * footprint.rowPitch, data + size_t(y) * rowBytes, rowBytes);
            g_rhi->CopyUploadToTexture(texture.texture, 0, upload.offset, footprint.rowPitch);
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

        // The texture of fetch constant `fetch` (null texture: transparent black).
        // `hdScale`: guest texels per host texel of an HD replacement (1 without one).
        rhi::TextureView GetTextureView(const xenos::TextureFetch& fetch, float* hdScale = nullptr)
        {
            if (hdScale)
                *hdScale = 1.0f;
            if (fetch.Type() != 2)
                return {};
            g_work.textureLookups++;

            if (ResolveTarget* resolvedTarget = FindResolveTarget(fetch.BaseAddress()))
            {
                ResolveSurface* resolved = &resolvedTarget->Current();
                if (!resolved->texture)
                    return {};
                uint32_t fetchFormat = fetch.Format();
                if (resolved->packed && fetchFormat != uint32_t(xenos::TextureFormat::k_24_8) &&
                    fetchFormat != uint32_t(xenos::TextureFormat::k_24_8_FLOAT))
                    return { resolved->packed };
                return { resolved->texture };
            }

            GuestTextureFormat format;
            if (!DescribeTextureFormat(fetch, format))
            {
                static uint32_t logged;
                if (logged++ < 16)
                    Log("[gpu] unsupported texture format %u", fetch.Format());
                return {};
            }
            TextureLayout layout = ComputeTextureLayout(fetch, format);
            if (!layout.widthBlocks || !layout.heightBlocks || layout.sizeBytes > (64u << 20) ||
                uint64_t(layout.baseAddress & 0x1FFFFFFF) + layout.sizeBytes > PHYSICAL_SIZE)
                return {};

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
                entry.texture = CreateTexture(width, height, levels, format.hostFormat, rhi::USAGE_SAMPLED, "guest texture");
                if (!entry.texture)
                {
                    Log("[gpu] could not create a %ux%u texture (format %u)", width, height, fetch.Format());
                    g_textures.erase(it);
                    return {};
                }
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

                    const rhi::TextureDesc& desc = entry.texture->desc;
                    rhi::Footprint footprints[16];
                    uint64_t levelOffsets[16];
                    levels = std::min(levels, 16u);
                    uint64_t totalBytes = UploadLayout(desc.format, desc.width, desc.height, levels, footprints, levelOffsets);
                    rhi::Device::Upload upload = AllocateUpload(totalBytes, rhi::UPLOAD_PLACEMENT_ALIGNMENT);
                    if (!upload.cpu)
                        return {};
                    // HD packs key the decoded base level; it is decoded into host memory first,
                    // since the upload memory is write-combined (very slow to read back).
                    bool hdKeyed = (hd::Active() || hd::Dumping()) && hd::Replaceable(format.hostFormat);
                    // The game's button icons are small 8-bit index textures (prompt_icons.h).
                    bool iconKeyed = g_promptIcons && format.hostFormat == DXGI_FORMAT_R8_UNORM && width <= 128 && height <= 128;
                    g_hdKeyScratch.clear();
                    for (uint32_t level = 0; level < levels; level++)
                    {
                        const TextureLayout& levelLayout = levelLayouts[level];
                        uint8_t* destination = upload.cpu + levelOffsets[level];
                        uint32_t rowBytes, rows;
                        HostTextureRows(levelLayout, format, rowBytes, rows);
                        if (rows > footprints[level].rows || rowBytes > footprints[level].rowPitch)
                            continue;  // not expected: the level layouts are sized from the host levels
                        if (g_textureScratch.size() < levelLayout.sizeBytes)
                            g_textureScratch.resize(levelLayout.sizeBytes);
                        if (level == 0 && (hdKeyed || iconKeyed))
                        {
                            g_hdKeyScratch.resize(size_t(footprints[level].rowPitch) * rows);
                            DecodeTexture(g_memory.Translate(levelLayout.baseAddress), g_textureScratch.data(), levelLayout, format, fetch.Endianness(),
                                          g_hdKeyScratch.data(), footprints[level].rowPitch);
                            memcpy(destination, g_hdKeyScratch.data(), g_hdKeyScratch.size());
                        }
                        else
                            DecodeTexture(g_memory.Translate(levelLayout.baseAddress), g_textureScratch.data(), levelLayout, format, fetch.Endianness(),
                                          destination, footprints[level].rowPitch);
                        g_rhi->CopyUploadToTexture(entry.texture, level, upload.offset + levelOffsets[level], footprints[level].rowPitch);
                    }
                    g_stats.textureUploads++;
                    g_work.textureUploads++;

                    if (iconKeyed && !g_hdKeyScratch.empty())
                    {
                        uint32_t rowBytes, rows;
                        HostTextureRows(levelLayouts[0], format, rowBytes, rows);
                        uint64_t key = hd::Key(g_hdKeyScratch.data(), footprints[0].rowPitch, rowBytes, rows, format.hostFormat, width, height);
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
                        uint32_t rowPitch = footprints[0].rowPitch;
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
                g_drawIcon = entry.icon;
                uint32_t style = IconStyle();
                if (style != entry.iconStyle)
                {
                    entry.iconStyle = style;
                    entry.iconTexture = style ? FindIconTexture(entry, style) : nullptr;
                }
                if (entry.iconTexture)
                {
                    g_drawIconPalette = entry.iconTexture->palette.texture;
                    return { entry.iconTexture->indices.texture };
                }
            }
            if (entry.hd)
            {
                if (hdScale)
                    *hdScale = entry.hdScale;
                return { entry.hd->texture };
            }
            return { entry.texture };
        }

        // A sampler for an HD replacement `1 / hdScale` times larger than the guest texture: the
        // guest's mip range shifted to the same texel sizes. A base-map-only sampler may still use
        // the HD levels down to the guest's size, so minification doesn't alias more than the guest.
        void AdjustSamplerForHd(rhi::SamplerDesc& desc, float hdScale)
        {
            constexpr float NO_LIMIT = 3.402823466e+38f;
            float shift = std::log2(1.0f / hdScale);
            if (desc.maxLod == 0.0f)
            {
                desc.maxLod = shift;
                if (desc.anisotropy <= 1)
                    desc.mipFilter = rhi::Filter::Linear;  // linear between mips
            }
            else if (desc.maxLod < NO_LIMIT)
                desc.maxLod += shift;
            if (desc.minLod > 0.0f)
                desc.minLod += shift;
        }

        rhi::Address AddressMode(uint32_t clamp)
        {
            switch (xenos::ClampMode(clamp))
            {
            case xenos::ClampMode::Repeat: return rhi::Address::Wrap;
            case xenos::ClampMode::MirroredRepeat: return rhi::Address::Mirror;
            case xenos::ClampMode::MirrorClampToEdge:
            case xenos::ClampMode::MirrorClampToHalfway: return rhi::Address::MirrorOnce;
            case xenos::ClampMode::ClampToBorder:
            case xenos::ClampMode::MirrorClampToBorder: return rhi::Address::Border;
            default: return rhi::Address::Clamp;
            }
        }

        // isTexture: a texture from guest memory (not a resolve target); the anisotropic filtering
        // option applies to those when they are filtered linearly.
        rhi::SamplerDesc MakeSampler(const xenos::TextureFetch& fetch, const TextureBinding* binding, bool isTexture)
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
            rhi::SamplerDesc desc;
            bool linearMag = mag == 1, linearMin = min == 1, linearMip = mip == 1;
            uint32_t guestAnisotropy = aniso > 1 ? 1u << std::min(aniso - 1, 4u) : 1u;
            uint32_t anisotropy = guestAnisotropy;
            if (isTexture && linearMin && linearMag)
                anisotropy = std::max(anisotropy, uint32_t(g_anisotropy));
            if (anisotropy > 1)
            {
                desc.anisotropy = uint8_t(anisotropy);
                desc.minFilter = desc.magFilter = desc.mipFilter = rhi::Filter::Linear;
            }
            else
            {
                desc.minFilter = linearMin ? rhi::Filter::Linear : rhi::Filter::Point;
                desc.magFilter = linearMag ? rhi::Filter::Linear : rhi::Filter::Point;
                desc.mipFilter = linearMip ? rhi::Filter::Linear : rhi::Filter::Point;
            }
            desc.addressU = AddressMode(fetch.ClampX());
            desc.addressV = AddressMode(fetch.ClampY());
            desc.addressW = AddressMode(fetch.ClampZ());
            desc.border = fetch.BorderColor() == 0 ? rhi::BorderColor::TransparentBlack
                          : fetch.BorderColor() == 1 ? rhi::BorderColor::OpaqueWhite : rhi::BorderColor::OpaqueBlack;
            // Mip filter 2 = base map: no mips. Otherwise the fetch constant's level range.
            desc.minLod = float(fetch.MipMinLevel());
            desc.maxLod = mip == 2 ? 0.0f : float(std::max(fetch.MipMaxLevel(), fetch.MipMinLevel()));
            if (mip == 2)
                desc.minLod = 0.0f;
            return desc;
        }

        // Pipelines -----------------------------------------------------------------------------

        rhi::SamplerDesc LinearClamp()
        {
            rhi::SamplerDesc sampler;
            sampler.minFilter = sampler.magFilter = sampler.mipFilter = rhi::Filter::Linear;
            return sampler;
        }

        // Resolve and present passes: one texture, 8 dwords of constants, a linear clamp sampler.
        rhi::Pipeline* CreateBlitPipeline(const char* name, const char* ps, DXGI_FORMAT target, uint32_t textures = 1, uint32_t constantBytes = 32)
        {
            rhi::PassPipelineDesc desc;
            desc.name = name;
            desc.ps = ps;
            desc.targets[0] = target;
            desc.textureCount = textures;
            desc.constantBytes = constantBytes;
            desc.samplers[0] = LinearClamp();
            desc.samplerCount = 1;
            rhi::Pipeline* pipeline = g_rhi->CreatePassPipeline(desc);
            if (!pipeline)
                Log("[gpu] could not create the %s pipeline for DXGI format %u", name, uint32_t(target));
            return pipeline;
        }

        rhi::Pipeline* GetResolvePipeline(DXGI_FORMAT format)
        {
            auto it = g_resolvePipelines.find(uint32_t(format));
            if (it != g_resolvePipelines.end())
                return it->second;
            rhi::Pipeline* pipeline = CreateBlitPipeline("resolve", RESOLVE_PS, format);
            g_resolvePipelines[uint32_t(format)] = pipeline;
            return pipeline;
        }

        rhi::Pipeline* GetResolveDepthPackedPipeline()
        {
            static bool tried;
            if (!tried)
            {
                tried = true;
                g_resolveDepthPackedPipeline = CreateBlitPipeline("resolve_depth_packed", RESOLVE_DEPTH_PACKED_PS, DXGI_FORMAT_R8G8B8A8_UNORM, 2);
            }
            return g_resolveDepthPackedPipeline;
        }

        rhi::Pipeline* GetPresentPipeline(DXGI_FORMAT format)
        {
            auto it = g_presentPipelines.find(uint32_t(format));
            if (it != g_presentPipelines.end())
                return it->second;
            rhi::Pipeline* pipeline = CreateBlitPipeline("present", PRESENT_PS, format, 1, PRESENT_CONSTANTS * 4);
            g_presentPipelines[uint32_t(format)] = pipeline;
            return pipeline;
        }

        // Scissor (window scissor + offset, intersected with the screen scissor), clamped to the
        // render target. As in Xenia's draw_util::GetScissor.
        rhi::Rect GuestScissor(uint32_t width, uint32_t height)
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
            return rhi::Rect{ x0, y0, x1, y1 };
        }

        // Wide screens (widescreen.h) -----------------------------------------------------------
        //
        // The host targets are g_stretch times wider than the guest's 16:9, so the game's 2D (laid
        // out in 1280x720) would be stretched. Draws of 2D (W = 1, checked in the vertex shader)
        // into the screen's 1x targets (1280 wide) are put back into the middle 16:9 area by
        // xe_hud_transform (shader_translator.cpp): scaled around the centre, except coordinates at
        // the screen's edges, so full-screen quads (fades, screen-space passes, backgrounds) still
        // cover the screen; a background (a full-screen quad of a texture that isn't a render
        // target) zooms to fill it. The Bink videos (while cutscenes::VideoPlaying) are just
        // scaled, between black bars.
        constexpr uint32_t HUD_WIDTH = 1280, HUD_HEIGHT = 720;
        constexpr uint64_t BINK_PS = 0xbd65c6f2d6f2d6d4ull;  // shaders\binkdecompress

        struct HudDraw
        {
            uint32_t mode = 0;  // xe_hud_transform's bits; 0 = not transformed
            float scaleX = 1.0f, scaleY = 1.0f;
            float shift = 0.0f;  // anchored HUD: guest pixels a corner element moves out (signed by its side)
            bool video = false;
        };
        HudDraw g_drawHud;  // of the draw being set up (HudScissor)

        HudDraw HudTransform(bool sceneDraw, uint32_t pitch, uint32_t msaa, uint64_t psHash, bool readsResolve)
        {
            HudDraw hud;
            // The Bink frame is drawn into the scene's (MSAA) target, not a 2D one: it is a video too.
            if (g_stretch == 1.0f || ((sceneDraw || msaa) && psHash != BINK_PS) || pitch != HUD_WIDTH)
                return g_drawHud = hud;
            hud.scaleX = std::min(1.0f, 1.0f / g_stretch);
            hud.scaleY = std::min(1.0f, g_stretch);
            // A video: the game draws the decoded Bink frame as a full-screen 2D quad.
            hud.video = psHash == BINK_PS || (cutscenes::VideoPlaying() && !readsResolve);
            // The game's own 4:3 (widescreen::GameAspect) laid its 2D out for the picture: only the
            // videos (16:9) go between bars.
            if (!hud.video && widescreen::GameAspect() != widescreen::BASE_ASPECT)
                return g_drawHud = HudDraw{};
            if (hud.video)
                hud.mode = 4;
            else
                hud.mode = readsResolve ? 1u : 3u;
            // Mode bit 8 (the HUD corners at the screen's edges, the old video.hud_16_9 = false) is
            // no longer set: the HUD always stays in the 16:9 area.
            return g_drawHud = hud;
        }

        // The game's 2D vertex shader: position = (x, y, z, 1) * c4..c7 (rows).
        constexpr uint64_t HUD_VS = 0xaecab5202246c470ull;

        // c4..c7 of the vertex shader (the 2D matrix of HUD_VS).
        void LoadHudMatrix(float c[16])
        {
            uint32_t base = Reg(xenos::REG_SQ_VS_CONST) & 0x1FF;
            for (uint32_t i = 0; i < 16; i++)
            {
                uint32_t bits = LoadBE32(GPU_MMIO_BEGIN + (xenos::REG_SHADER_CONSTANT_000_X + (base + 4) * 4 + i) * 4);
                memcpy(&c[i], &bits, 4);
            }
        }

        // The game's own 2D (orthographic, W = 1): its scissor is in 16:9 space and moves with it.
        // 2D drawn through a pure3d camera (W != 1, the frontend; HUD_GS) was placed by the Hor+
        // projection, and the game computes its scissor from that projection: already in place
        // (squeezing it again cut the HUB's carousel of titans); so is any other vertex shader's
        // (3D, like the HUB's titan dolls).
        bool HudDrawIsOrtho(uint64_t vsHash)
        {
            if (vsHash != HUD_VS)
                return false;
            float c[16];
            LoadHudMatrix(c);
            return c[3] == 0.0f && c[7] == 0.0f && c[11] == 0.0f && c[15] == 1.0f;
        }

        // Where the draw of a button icon lands (through the game's 2D vertex shader, c4..c7):
        // its box, 0..1 of the picture, for the clickable prompts (prompt_icons.h IconSpots).
        void NoteIconSpot(const float* data, uint32_t startDwords, uint32_t dwords, uint32_t stride, uint32_t minIndex, uint32_t maxIndex,
                          const SystemConstants& system, float targetWidth, float targetHeight, const HudDraw& hud)
        {
            if (!data || maxIndex < minIndex || maxIndex - minIndex > 64 || g_iconSpots.size() >= 32)
                return;
            float c[16];
            LoadHudMatrix(c);
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            bool ortho = true;
            for (uint32_t v = minIndex; v <= maxIndex; v++)
            {
                int64_t at = int64_t(v) * stride - int64_t(startDwords);
                if (at < 0 || at + 3 > int64_t(dwords))
                    return;
                float x = data[at], y = data[at + 1], z = data[at + 2];
                float clipX = x * c[0] + y * c[4] + z * c[8] + c[12];
                float clipY = x * c[1] + y * c[5] + z * c[9] + c[13];
                float clipW = x * c[3] + y * c[7] + z * c[11] + c[15];
                if (!(fabsf(clipW) > 1e-6f))
                    return;
                ortho &= fabsf(clipW - 1.0f) <= 1e-5f;
                float ndcX = clipX / clipW * system.ndcScale[0] + system.ndcOffset[0];
                float ndcY = clipY / clipW * system.ndcScale[1] + system.ndcOffset[1];
                float u = (ndcX + 1.0f) * 0.5f * targetWidth / float(HUD_WIDTH), w = (1.0f - ndcY) * 0.5f * targetHeight / float(HUD_HEIGHT);
                x0 = std::min(x0, u), x1 = std::max(x1, u), y0 = std::min(y0, w), y1 = std::max(y1, w);
            }
            // The game's own 2D goes into the middle 16:9 area (HUD_GS); the frontend's is in place.
            if (ortho && hud.mode && !(hud.mode & 4u))
                x0 = 0.5f + (x0 - 0.5f) * hud.scaleX, x1 = 0.5f + (x1 - 0.5f) * hud.scaleX;
            if (x1 > x0 && y1 > y0 && x1 > 0.0f && x0 < 1.0f && y1 > 0.0f && y0 < 1.0f)
                g_iconSpots.push_back({ g_drawIcon, x0, y0, x1, y1 });
        }

        // The frontend's page (640 x 480) drawn through a pure3d camera this frame: the 2D vertex
        // shader with W from z (W = z c11 + c15) the same, and not 1, at every vertex (0.5 for the
        // page). Only the frontend's screens draw it (not the pause in a level): for the menus'
        // mouse (menu_mouse.h NoteFrontendPage).
        void NotePage(const float* data, uint32_t startDwords, uint32_t dwords, uint32_t stride, uint32_t minIndex, uint32_t maxIndex)
        {
            if (g_pageNoted || !data || maxIndex < minIndex || maxIndex - minIndex > 4096)
                return;
            float c[16];
            LoadHudMatrix(c);
            if (c[3] != 0.0f || c[7] != 0.0f)
                return;
            float w = 0.0f;
            for (uint32_t v = minIndex; v <= maxIndex; v++)
            {
                int64_t at = int64_t(v) * stride - int64_t(startDwords);
                if (at < 0 || at + 3 > int64_t(dwords))
                    return;
                float vw = data[at + 2] * c[11] + c[15];
                if (v == minIndex)
                    w = vw;
                else if (fabsf(vw - w) > 1e-5f)
                    return;
            }
            g_pageNoted = w > 1e-4f && fabsf(w - 1.0f) > 1e-5f;
        }

        // HUD_GS worked out on the CPU for the game's 2D vertex shader: per triangle, HUD_GS moves
        // each axis by one scale and offset (or leaves it). When every triangle of the draw gets the
        // same ones, they are folded into the draw's copy of c4..c7, so the 2D needs no geometry
        // shader: on Adreno (Turnip) HUD_GS took 72 of the 83 ms of a menu frame, and Mali has no
        // geometry shaders. The draw's triangles treated differently (or a draw it can't read):
        // HUD_GS as before. CRASH_HUD_FOLD=0 turns it off (comparisons).
        struct HudFold
        {
            float scale[2] = { 1.0f, 1.0f }, offset[2] = { 0.0f, 0.0f };  // NDC' = NDC * scale + offset
        };

        bool HudFoldDraw(const float* data, uint32_t startDwords, uint32_t dwords, uint32_t stride, const std::vector<uint32_t>& indices,
                         uint32_t minIndex, uint32_t maxIndex, rhi::Topology topology, const SystemConstants& system, float targetWidth, float targetHeight,
                         const HudDraw& hud, HudFold& fold)
        {
            constexpr uint32_t CUT = 0xFFFFFFFF;
            if (!data || maxIndex < minIndex || maxIndex - minIndex > 4096 || indices.size() > 16384 ||
                (system.flags & (FLAG_XY_DIVIDED_BY_W | FLAG_W_NOT_RECIPROCAL)) != FLAG_W_NOT_RECIPROCAL ||
                system.ndcScale[0] == 0.0f || system.ndcScale[1] == 0.0f)
                return false;
            float c[16];
            LoadHudMatrix(c);
            // Each vertex as HUD_GS sees it: guest pixels of the target and W.
            struct Vertex { float x, y, w; };
            static std::vector<Vertex> vertices;
            vertices.resize(maxIndex - minIndex + 1);
            for (uint32_t v = minIndex; v <= maxIndex; v++)
            {
                int64_t at = int64_t(v) * stride - int64_t(startDwords);
                if (at < 0 || at + 3 > int64_t(dwords))
                    return false;
                float x = data[at], y = data[at + 1], z = data[at + 2];
                float clipX = x * c[0] + y * c[4] + z * c[8] + c[12];
                float clipY = x * c[1] + y * c[5] + z * c[9] + c[13];
                float clipW = x * c[3] + y * c[7] + z * c[11] + c[15];
                if (!(fabsf(clipW) > 1e-6f))
                    return false;
                float ndcX = clipX / clipW * system.ndcScale[0] + system.ndcOffset[0];
                float ndcY = clipY / clipW * system.ndcScale[1] + system.ndcOffset[1];
                vertices[v - minIndex] = { (ndcX + 1.0f) * 0.5f * targetWidth, (1.0f - ndcY) * 0.5f * targetHeight, clipW };
            }

            const float sx = hud.scaleX, sy = hud.scaleY, cx = float(HUD_WIDTH) * 0.5f, cy = float(HUD_HEIGHT) * 0.5f;
            const uint32_t mode = hud.mode;
            bool first = true;
            float k[2] = {}, m[2] = {};  // result = px * k + m, guest pixels
            auto triangle = [&](uint32_t i0, uint32_t i1, uint32_t i2) -> bool {
                if (i0 == CUT || i1 == CUT || i2 == CUT || i0 == i1 || i1 == i2 || i0 == i2)
                    return true;  // draws nothing
                const Vertex& a = vertices[i0 - minIndex];
                const Vertex& b = vertices[i1 - minIndex];
                const Vertex& d = vertices[i2 - minIndex];
                if ((b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x) == 0.0f)
                    return true;
                float tk[2] = { 1.0f, 1.0f }, tm[2] = { 0.0f, 0.0f };
                float w = a.w;
                bool flat = w > 0.0f && fabsf(b.w - w) <= 1e-5f * w && fabsf(d.w - w) <= 1e-5f * w;
                if (flat)
                {
                    bool projected = fabsf(w - 1.0f) > 1e-5f;
                    float halfX = projected ? cx * std::min(sx, 1.0f) : cx, halfY = projected ? cy * std::min(sy, 1.0f) : cy;
                    float loX = std::min({ a.x, b.x, d.x }), hiX = std::max({ a.x, b.x, d.x });
                    float loY = std::min({ a.y, b.y, d.y }), hiY = std::max({ a.y, b.y, d.y });
                    bool fullX = loX <= cx - halfX + 2.0f && hiX >= cx + halfX - 2.0f;
                    bool fullY = loY <= cy - halfY + 2.0f && hiY >= cy + halfY - 2.0f;
                    if (projected)
                    {
                        bool past = loX < cx - halfX - 2.0f || loY < cy - halfY - 2.0f || hiX > cx + halfX + 2.0f || hiY > cy + halfY + 2.0f;
                        bool background = (fullX && fullY) || (past && hiX - loX > halfX * 0.5f && hiY - loY > halfY * 0.5f);
                        if ((mode & 2u) && !(mode & 4u) && background)
                            tk[0] = tk[1] = 1.0f / std::min(sx, sy);
                    }
                    else
                    {
                        tk[0] = sx, tk[1] = sy;
                        if (!(mode & 4u))
                        {
                            if ((mode & 8u) && !fullX)
                                tm[0] = hud.shift;
                            if (fullX) tk[0] = 1.0f, tm[0] = 0.0f;
                            if (fullY) tk[1] = 1.0f, tm[1] = 0.0f;
                            if ((mode & 2u) && fullX && fullY)
                                tk[0] = 1.0f / sy, tk[1] = 1.0f / sx, tm[0] = tm[1] = 0.0f;
                        }
                    }
                    // Scaled around the centre.
                    tm[0] += (1.0f - tk[0]) * cx;
                    tm[1] += (1.0f - tk[1]) * cy;
                }
                if (first)
                {
                    first = false;
                    k[0] = tk[0], k[1] = tk[1], m[0] = tm[0], m[1] = tm[1];
                    return true;
                }
                return tk[0] == k[0] && tk[1] == k[1] && tm[0] == m[0] && tm[1] == m[1];
            };
            if (topology == rhi::Topology::TriangleList)
            {
                for (size_t i = 0; i + 2 < indices.size(); i += 3)
                    if (!triangle(indices[i], indices[i + 1], indices[i + 2]))
                        return false;
            }
            else if (topology == rhi::Topology::TriangleStrip)
            {
                for (size_t i = 0; i + 2 < indices.size(); i++)
                    if (!triangle(indices[i], indices[i + 1], indices[i + 2]))
                        return false;
            }
            else
                return false;
            if (first)
                return false;
            // Guest pixels -> NDC of the target.
            fold.scale[0] = k[0];
            fold.offset[0] = k[0] - 1.0f + 2.0f * m[0] / targetWidth;
            fold.scale[1] = k[1];
            fold.offset[1] = 1.0f - k[1] - 2.0f * m[1] / targetHeight;
            return true;
        }

        // The fold in the vertex shader's copy of c4..c7 (rows; `constants` from c0): NDC = clip / W *
        // ndcScale + ndcOffset, so clip' = clip * scale + W * ((scale - 1) * ndcOffset + offset) / ndcScale.
        void ApplyHudFold(float* constants, const HudFold& fold, const SystemConstants& system)
        {
            float* c = constants + 16;
            for (int axis = 0; axis < 2; axis++)
            {
                float a = fold.scale[axis];
                float b = ((a - 1.0f) * system.ndcOffset[axis] + fold.offset[axis]) / system.ndcScale[axis];
                for (int row = 0; row < 4; row++)
                    c[row * 4 + axis] = c[row * 4 + axis] * a + c[row * 4 + 3] * b;
            }
        }

        float HudCoordinate(float value, float size, float scale)
        {
            if (value <= 2.0f || value >= size - 2.0f)
                return value;
            return size * 0.5f + (value - size * 0.5f) * scale;
        }

        // The draw's scissor (guest pixels) moved like its vertices (only the centred part: an
        // anchored HUD element keeps the scissor of the centre).
        void HudScissor(rhi::Rect& scissor)
        {
            const HudDraw& hud = g_drawHud;
            scissor.left = int32_t(floorf(HudCoordinate(float(scissor.left), float(HUD_WIDTH), hud.scaleX)));
            scissor.right = int32_t(ceilf(HudCoordinate(float(scissor.right), float(HUD_WIDTH), hud.scaleX)));
            scissor.top = int32_t(floorf(HudCoordinate(float(scissor.top), float(HUD_HEIGHT), hud.scaleY)));
            scissor.bottom = int32_t(ceilf(HudCoordinate(float(scissor.bottom), float(HUD_HEIGHT), hud.scaleY)));
            if (hud.mode & 8u)
                scissor.left = 0, scissor.right = std::max(scissor.right, int32_t(HUD_WIDTH));
        }

        // A video frame: black where the 16:9 picture doesn't reach (the sides, or above and below).
        void ClearVideoBars(RenderTarget& target)
        {
            const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            float sx = XScale(target.scale), sy = target.scale;
            int32_t width = int32_t(ceilf(float(HUD_WIDTH) * sx)), height = int32_t(ceilf(float(HUD_HEIGHT) * sy));
            const HudDraw& hud = g_drawHud;
            if (hud.scaleX < 1.0f)
            {
                int32_t bar = int32_t(floorf(float(width) * (1.0f - hud.scaleX) * 0.5f));
                rhi::Rect left{ 0, 0, bar, height }, right{ width - bar, 0, width, height };
                g_rhi->ClearColor(target.texture, black, &left);
                g_rhi->ClearColor(target.texture, black, &right);
            }
            else if (hud.scaleY < 1.0f)
            {
                int32_t bar = int32_t(floorf(float(height) * (1.0f - hud.scaleY) * 0.5f));
                rhi::Rect top{ 0, 0, width, bar }, bottom{ 0, height - bar, width, height };
                g_rhi->ClearColor(target.texture, black, &top);
                g_rhi->ClearColor(target.texture, black, &bottom);
            }
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
            rhi::Pipeline* motionPipeline = nullptr;
            rhi::Pipeline* motionViewPipeline = nullptr;
            float showMotion = 0.0f;  // CRASH_GPU_SHOW_MOTION
            rhi::Texture* motion = nullptr;
            rhi::Texture* output = nullptr;
            uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
            int64_t lastTime{};
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
            if (t.motion && t.renderWidth == renderWidth && t.renderHeight == renderHeight && t.outputWidth == outputWidth &&
                t.outputHeight == outputHeight)
                return true;
            DestroyTexture(t.motion);
            DestroyTexture(t.output);
            t.motion = CreateTexture(renderWidth, renderHeight, 1, DXGI_FORMAT_R16G16_FLOAT, rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET, "motion vectors");
            if (!t.motion)
                return false;
            t.output = CreateTexture(outputWidth, outputHeight, 1, DXGI_FORMAT_R8G8B8A8_UNORM, rhi::USAGE_SAMPLED | rhi::USAGE_STORAGE, "upscaler output");
            if (!t.output)
            {
                DestroyTexture(t.motion);
                return false;
            }
            t.renderWidth = renderWidth;
            t.renderHeight = renderHeight;
            t.outputWidth = outputWidth;
            t.outputHeight = outputHeight;
            Log("[gpu] temporal upscaling (%s): scene %ux%u -> %ux%u", upscaler::ActiveName().c_str(), renderWidth, renderHeight, outputWidth, outputHeight);
            return true;
        }

        struct MotionConstants
        {
            float inverseViewProj[16];
            float previousViewProj[16];
            float renderSize[2];
            float zScale, zOffset;
            float velocityScale;
            uint32_t flags;
            float padding[2];
        };

        bool CreateTemporalPipeline()
        {
            rhi::PassPipelineDesc desc;
            desc.name = "motion_vectors";
            desc.ps = MOTION_PS;
            desc.targets[0] = DXGI_FORMAT_R16G16_FLOAT;
            desc.textureCount = 2;
            desc.constantBytes = sizeof(MotionConstants);
            g_temporal.motionPipeline = g_rhi->CreatePassPipeline(desc);
            if (!g_temporal.motionPipeline)
                return false;
            if (const char* show = getenv("CRASH_GPU_SHOW_MOTION"))
            {
                g_temporal.showMotion = float(atof(show));
                desc.name = "motion_view";
                desc.ps = MOTION_VIEW_PS;
                desc.targets[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.constantBytes = 16;
                g_temporal.motionViewPipeline = g_rhi->CreatePassPipeline(desc);
                if (!g_temporal.motionViewPipeline)
                    g_temporal.showMotion = 0.0f;
            }
            return true;
        }

        // Runs the upscaler on this frame's scene (see "Temporal upscaling" above).
        void RunTemporal()
        {
            ProfileLabel("scene effects");
            Temporal& t = g_temporal;
            t.done = true;
            ResolveTarget& color = *t.color;
            ResolveSurface& input = color.surfaces[SurfaceIndex(g_sceneScale)];
            ResolveSurface* depth = t.depth ? &t.depth->surfaces[SurfaceIndex(g_sceneScale)] : nullptr;
            ResolveSurface* velocity = t.velocity ? &t.velocity->surfaces[SurfaceIndex(g_sceneScale)] : nullptr;
            if (!input.texture || !depth || !depth->texture || t.depth->format != DXGI_FORMAT_R32_FLOAT)
                return;
            uint32_t renderWidth = input.hostWidth, renderHeight = input.hostHeight;
            uint32_t outputWidth = ScaledSize(color.width, XScale(g_renderScale)), outputHeight = ScaledSize(color.height, g_renderScale);
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
            MotionConstants constants{};
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
            constants.flags = (velocity && velocity->texture ? 1u : 0u) | (haveCamera ? 2u : 0u);
            rhi::PassDraw motionPass;
            motionPass.pipeline = t.motionPipeline;
            motionPass.targets[0] = t.motion;
            motionPass.viewport = { 0.0f, 0.0f, float(renderWidth), float(renderHeight) };
            motionPass.scissor = { 0, 0, int32_t(renderWidth), int32_t(renderHeight) };
            motionPass.textures[0] = { depth->texture };
            motionPass.textures[1] = { (constants.flags & 1u) ? velocity->texture : nullptr };
            motionPass.textureCount = 2;
            motionPass.constants = &constants;
            motionPass.constantBytes = sizeof(constants);
            g_rhi->DrawPass(motionPass);

            // Lighting remaster, on the scene at its own resolution (before the upscaler accumulates it).
            if (g_lightingOptions.Any() && haveCamera && t.showMotion == 0.0f)
            {
                lighting::Frame frame;
                frame.color = input.texture;
                frame.depth = depth->texture;
                frame.motion = t.motion;
                frame.width = renderWidth;
                frame.height = renderHeight;
                frame.viewProj = viewProj;
                frame.zScale = constants.zScale;
                frame.zOffset = constants.zOffset;
                frame.reset = reset;
                frame.frame = g_frame;
                if (lighting::Apply(frame, g_lightingOptions))
                    color.lastWriteFrame = g_frame;
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
            int64_t now, frequency;
            now = int64_t(plat::PerfCounter());
            frequency = int64_t(plat::PerfFrequency());
            float frameMs = t.lastTime ? float(double(now - t.lastTime) * 1000.0 / double(frequency)) : 33.3f;
            t.lastTime = now;
            upscaler::Frame frame;
            frame.color = input.texture;
            frame.depth = depth->texture;
            frame.motion = t.motion;
            frame.output = t.output;
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
                float viewConstants[4] = { t.showMotion };
                rhi::PassDraw viewPass;
                viewPass.pipeline = t.motionViewPipeline;
                viewPass.targets[0] = view.texture;
                viewPass.viewport = { 0.0f, 0.0f, float(renderWidth), float(renderHeight) };
                viewPass.scissor = { 0, 0, int32_t(renderWidth), int32_t(renderHeight) };
                viewPass.textures[0] = { t.motion };
                viewPass.textures[1] = { (constants.flags & 1u) ? velocity->texture : nullptr };
                viewPass.textureCount = 2;
                viewPass.constants = viewConstants;
                viewPass.constantBytes = sizeof(viewConstants);
                g_rhi->DrawPass(viewPass);
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
            int64_t evaluateStart, evaluateEnd;
            evaluateStart = int64_t(plat::PerfCounter());
            ok = upscaler::Evaluate(frame);
            evaluateEnd = int64_t(plat::PerfCounter());
            t.evaluateMs += double(evaluateEnd - evaluateStart) * 1000.0 / double(frequency);
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
            rhi::Rect region{ 0, 0, int32_t(std::min(outputWidth, result.hostWidth)), int32_t(std::min(outputHeight, result.hostHeight)) };
            g_rhi->CopyTexture(result.texture, t.output, &region);
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
            upscaler::Jitter(nextFrame, ScaledSize(1280, XScale(g_sceneScale)), ScaledSize(1280, XScale(g_renderScale)), t.jitterX, t.jitterY);
            t.jitterGuestX = t.jitterX / XScale(g_sceneScale);
            t.jitterGuestY = t.jitterY / g_sceneScale;
        }

        void Resolve()
        {
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
            rhi::Rect scissor = GuestScissor(16383, 16383);
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
            // CRASH_GPU_TRACE_RESOLVE=<physical address, hex>: every resolve into that address.
            static const uint32_t s_traceResolve = getenv("CRASH_GPU_TRACE_RESOLVE") ? uint32_t(strtoul(getenv("CRASH_GPU_TRACE_RESOLVE"), nullptr, 16)) : 0;
            bool logThis = loggedResolves < 40 || TracingSwap() || (s_traceResolve && (Reg(xenos::REG_RB_COPY_DEST_BASE) & 0x1FFFFFFF) == s_traceResolve);

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

                ProfileLabel("resolve %s EDRAM %u -> 0x%08X", isDepth ? "depth" : "color",
                             (isDepth ? depthInfo : Reg(xenos::ColorInfoRegister(sourceSelect))) & 0xFFF, destBase);
                ResolveSurface* surface = &EnsureSurface(*destination, scale);
                destination->current = SurfaceIndex(scale);
                if (logThis && TracingSwap())
                    Log("[gpu]   -> texture %p (surface %u), source texture %p", (void*)surface->texture, SurfaceIndex(scale), (void*)source->texture);
                NoteSceneResolve(*destination, *source, isDepth, sourceSelect);
                rhi::Rect hostRect{ std::max(0, ScaledFloor(float(x0 + dx), XScale(scale))), std::max(0, ScaledFloor(float(y0 + dy), scale)),
                                    std::min(int32_t(surface->hostWidth), ScaledCeil(float(x1 + dx), XScale(scale))),
                                    std::min(int32_t(surface->hostHeight), ScaledCeil(float(y1 + dy), scale)) };
                rhi::PassDraw pass;
                pass.viewport = { 0.0f, 0.0f, float(surface->hostWidth), float(surface->hostHeight) };
                pass.scissor = hostRect;
                uint32_t constants[8] = {};
                pass.constants = constants;
                pass.constantBytes = sizeof(constants);
                bool visible = hostRect.left < hostRect.right && hostRect.top < hostRect.bottom;
                if (rhi::Pipeline* pipeline = GetResolvePipeline(format))
                {
                    float expScale = ldexpf(1.0f, expBias);
                    constants[0] = uint32_t(ScaledFloor(float(dx), XScale(scale)));
                    constants[1] = uint32_t(ScaledFloor(float(dy), scale));
                    memcpy(&constants[2], &expScale, 4);
                    constants[3] = uint32_t(swapRedBlue) | (samplesX == 2 ? 4u : 0u) | RowScaleFlags(rowScale);
                    pass.pipeline = pipeline;
                    pass.targets[0] = surface->texture;
                    pass.textures[0] = { source->texture, isDepth ? rhi::View::Depth : rhi::View::Default };
                    pass.textureCount = 1;
                    if (visible)
                        g_rhi->DrawPass(pass);
                    target->lastWriteFrame = g_frame;
                }
                if (!isDepth && ProbingSwap(g_stats.swaps + 1))
                {
                    char tag[96];
                    snprintf(tag, sizeof(tag), "swap %llu resolve EDRAM %u -> 0x%08X", (unsigned long long)g_stats.swaps + 1,
                             Reg(xenos::ColorInfoRegister(sourceSelect)) & 0xFFF, destBase);
                    g_rhi->Probe(source->texture, std::string(tag) + " source");
                    g_rhi->Probe(surface->texture, std::string(tag) + " destination");
                }

                // CRASH_GPU_DUMP_RESOLVE=<physical address, hex>: in the traced frames (CRASH_GPU_TRACE_SWAP),
                // the destination after each resolve into that address, and its source, as PNGs.
                static const uint32_t s_dumpResolve = getenv("CRASH_GPU_DUMP_RESOLVE") ? uint32_t(strtoul(getenv("CRASH_GPU_DUMP_RESOLVE"), nullptr, 16)) : 0;
                if (s_dumpResolve && destBase == s_dumpResolve && TracingSwap() && !isDepth)
                {
                    static uint32_t dumped;
                    for (int which = 0; which < 2; which++)
                    {
                        std::vector<uint8_t> pixels;
                        uint32_t width, height;
                        rhi::Texture* texture = which ? source->texture : surface->texture;
                        if (texture->desc.format != DXGI_FORMAT_R8G8B8A8_UNORM || !g_rhi->ReadBack(texture, pixels, width, height))
                            continue;
                        for (size_t i = 3; i < pixels.size(); i += 4)
                            pixels[i] = 255;
                        char name[96];
                        snprintf(name, sizeof(name), "resolve_%08X_swap%llu_%u_%s.png", destBase, (unsigned long long)g_stats.swaps + 1, dumped,
                                 which ? "source" : "destination");
                        WritePng(g_screenshotDir / name, pixels.data(), width, height, width * 4);
                    }
                    dumped++;
                }

                if (isDepth && target->format == DXGI_FORMAT_R32_FLOAT)
                {
                    if (rhi::Pipeline* packedPipeline = GetResolveDepthPackedPipeline())
                    {
                        EnsurePackedDepth(*surface);
                        bool isFloat = source->guestFormat == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
                        constants[0] = uint32_t(ScaledFloor(float(dx), XScale(scale)));
                        constants[1] = uint32_t(ScaledFloor(float(dy), scale));
                        constants[2] = 0;
                        constants[3] = (isFloat ? 2u : 0u) | (samplesX == 2 ? 4u : 0u) | RowScaleFlags(rowScale);
                        pass.pipeline = packedPipeline;
                        pass.targets[0] = surface->packed;
                        pass.textures[0] = { source->texture, rhi::View::Depth };
                        pass.textures[1] = { source->texture, rhi::View::Stencil };
                        pass.textureCount = 2;
                        if (visible)
                            g_rhi->DrawPass(pass);
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
            ProfileLabel("resolve clear");
            rhi::Rect clearRect{ ScaledFloor(float(x0 * int32_t(samplesX)), XScale(scale)), ScaledFloor(floorf(float(y0) * rowScale), scale),
                                  ScaledCeil(float(x1 * int32_t(samplesX)), XScale(scale)), ScaledCeil(ceilf(float(y1) * rowScale), scale) };
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
                g_rhi->ClearColor(source->texture, color, &clearRect);
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
                g_rhi->ClearDepthStencil(depth.texture, std::clamp(depthValue, 0.0f, 1.0f), uint8_t(value & 0xFF), &clearRect);
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

        // Rectangle lists without a geometry shader (RECT_LIST_GS; Mali): each rectangle (3 indices)
        // becomes two triangles whose 4th corner is a vertex made for it, numbered from `cornerBase`
        // (the draw's highest index + 1): `corners` gets a, b, c per rectangle (c at the right
        // angle, picked from the first fetch's x / y as RECT_LIST_GS does from the positions) for
        // the vertex upload, which writes a + b - c (FillRectCorners). False: no position to read.
        struct IndexData;
        bool ExpandRectList(const ShaderEntry* vs, IndexData& indexData, std::vector<uint32_t>& corners, uint32_t& cornerBase);

        // Expands the guest indices (DMA, immediate or auto) to final 32-bit vertex indices with
        // the offset/clamp/reset rules applied, then converts the primitive type for the host API.
        bool BuildIndices(const DrawPacket& packet, uint32_t primitive, uint32_t sourceSelect, bool index32, uint32_t count,
                          IndexData& out, rhi::Topology& topology, bool& rectList)
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
                topology = rhi::Topology::PointList;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::LineList:
                topology = rhi::Topology::LineList;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::LineStrip:
                topology = rhi::Topology::LineStrip;
                indices = raw;
                break;
            case xenos::PrimitiveType::LineLoop:
                topology = rhi::Topology::LineStrip;
                indices = raw;
                if (count)
                    indices.push_back(raw[0]);
                break;
            case xenos::PrimitiveType::TriangleList:
            case xenos::PrimitiveType::TriangleWithWFlags:
                topology = rhi::Topology::TriangleList;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::TriangleStrip:
            case xenos::PrimitiveType::QuadStrip:
                topology = rhi::Topology::TriangleStrip;
                indices.swap(raw);  // no copy: raw is refilled by the next draw
                break;
            case xenos::PrimitiveType::TriangleFan:
            case xenos::PrimitiveType::Polygon:
            {
                topology = rhi::Topology::TriangleList;
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
                topology = rhi::Topology::TriangleList;
                for (uint32_t i = 0; i + 3 < count; i += 4)
                    indices.insert(indices.end(), { raw[i], raw[i + 1], raw[i + 2], raw[i], raw[i + 2], raw[i + 3] });
                break;
            case xenos::PrimitiveType::RectangleList:
                topology = rhi::Topology::TriangleList;
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
            if (topology == rhi::Topology::TriangleList || topology == rhi::Topology::LineList ||
                topology == rhi::Topology::PointList)
                out.hasCut = false;
            return !indices.empty() && out.minIndex <= out.maxIndex;
        }

        bool ExpandRectList(const ShaderEntry* vs, IndexData& indexData, std::vector<uint32_t>& corners, uint32_t& cornerBase)
        {
            const VertexFetchBinding* position = nullptr;
            for (const VertexFetchBinding& binding : vs->info.vertexFetches)
                if (!position && binding.indexedByVertexIndex && binding.strideDwords >= 2 && binding.minOffsetDwords >= 0)
                    position = &binding;
            std::vector<uint32_t>& indices = indexData.indices;
            if (!position || indices.size() < 3)
            {
                static int logged;
                if (logged++ < 4)
                    Log("[gpu] rectangle list without a position to read (%zu indices): skipped", indices.size());
                return false;
            }
            uint32_t fc = position->fetchConstant;
            xenos::VertexFetch fetch{ { Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 2), Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 2 + 1) } };
            if (fetch.Type() != 3 || uint64_t(fetch.Address() & 0x1FFFFFFF) + uint64_t(fetch.SizeDwords()) * 4 > PHYSICAL_SIZE)
                return false;
            const uint32_t* data = reinterpret_cast<const uint32_t*>(g_memory.Translate(gpu::GuestFromGpuAddress(fetch.Address())));
            uint32_t size = fetch.SizeDwords(), endian = fetch.Endianness();
            auto xy = [&](uint32_t index, float& x, float& y) {
                uint64_t at = uint64_t(index) * position->strideDwords + uint32_t(position->minOffsetDwords);
                if (at + 2 > size)
                    return false;
                uint32_t bx = xenos::GpuSwap(data[at], endian), by = xenos::GpuSwap(data[at + 1], endian);
                memcpy(&x, &bx, 4);
                memcpy(&y, &by, 4);
                return true;
            };
            constexpr uint32_t CUT = 0xFFFFFFFF;
            static std::vector<uint32_t> expanded;
            expanded.clear();
            cornerBase = indexData.maxIndex + 1;
            uint32_t made = 0;
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                uint32_t v[3] = { indices[i], indices[i + 1], indices[i + 2] };
                float x[3], y[3];
                if (v[0] == CUT || v[1] == CUT || v[2] == CUT || !xy(v[0], x[0], y[0]) || !xy(v[1], x[1], y[1]) || !xy(v[2], x[2], y[2]))
                    continue;
                auto distance = [&](int a, int b) { return (x[b] - x[a]) * (x[b] - x[a]) + (y[b] - y[a]) * (y[b] - y[a]); };
                float d01 = distance(0, 1), d12 = distance(1, 2), d20 = distance(2, 0);
                int c = (d12 >= d01 && d12 >= d20) ? 0 : (d20 >= d01 ? 1 : 2);
                uint32_t a = v[(c + 1) % 3], b = v[(c + 2) % 3], corner = cornerBase + made++;
                expanded.insert(expanded.end(), { v[c], a, b, b, a, corner });
                corners.insert(corners.end(), { a, b, v[c] });
            }
            if (!made)
                return false;
            indices.swap(expanded);
            indexData.maxIndex = cornerBase + made - 1;
            return true;
        }

        // The made corners of ExpandRectList in one vertex fetch's upload (`data`: dwords from
        // `startDwords`, the guest's first `copyDwords`): per dword a + b - c as floats (positions,
        // coordinates), the value itself where the three agree (colours, any bits).
        void FillRectCorners(uint32_t* data, uint32_t startDwords, uint32_t copyDwords, uint32_t dwords, const VertexFetchBinding& binding,
                             const std::vector<uint32_t>& corners, uint32_t cornerBase)
        {
            uint32_t stride = binding.strideDwords;
            for (size_t r = 0; r * 3 + 2 < corners.size(); r++)
            {
                for (int32_t k = binding.minOffsetDwords; k < binding.maxEndDwords; k++)
                {
                    int64_t at[4];
                    uint32_t index[4] = { corners[r * 3], corners[r * 3 + 1], corners[r * 3 + 2], cornerBase + uint32_t(r) };
                    for (int j = 0; j < 4; j++)
                        at[j] = int64_t(index[j]) * stride + k - startDwords;
                    if (at[0] < 0 || at[1] < 0 || at[2] < 0 || at[0] >= copyDwords || at[1] >= copyDwords || at[2] >= copyDwords ||
                        at[3] < int64_t(copyDwords) || at[3] >= dwords)
                        continue;
                    uint32_t a = data[at[0]], b = data[at[1]], c = data[at[2]];
                    if (a == b && b == c)
                    {
                        data[at[3]] = a;
                        continue;
                    }
                    float fa, fb, fc;
                    memcpy(&fa, &a, 4);
                    memcpy(&fb, &b, 4);
                    memcpy(&fc, &c, 4);
                    float fd = fa + fb - fc;
                    memcpy(&data[at[3]], &fd, 4);
                }
            }
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
            int64_t now = int64_t(plat::PerfCounter());
            g_profileLast = uint64_t(now);
        }
        void ProfileMark(int bucket)
        {
            if (!g_profileFrame)
                return;
            int64_t now = int64_t(plat::PerfCounter());
            g_profileTicks[bucket] += uint64_t(now) - g_profileLast;
            g_profileLast = uint64_t(now);
        }

        void DrawImpl(const DrawPacket& packet)
        {
            // The draw's uploads (indices, vertices, constants) come before the draw is recorded:
            // held so a submission in between doesn't free them (rhi_ring.h).
            struct UploadHold
            {
                UploadHold() { g_rhi->HoldUploads(true); }
                ~UploadHold() { g_rhi->HoldUploads(false); }
            } hold;
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
            rhi::Topology topology;
            bool rectList;
            if (!BuildIndices(packet, primitive, sourceSelect, index32, count, indexData, topology, rectList))
            {
                Skip(SKIP_PRIMITIVE);
                return;
            }
            bool polygonal = topology == rhi::Topology::TriangleList || topology == rhi::Topology::TriangleStrip;
            if (polygonal && cullFront && cullBack)
            {
                Skip(SKIP_CULL);
                return;
            }
            // No geometry shader for rectangle lists (Mali): two triangles each (ExpandRectList).
            static std::vector<uint32_t> rectCorners;
            rectCorners.clear();
            uint32_t rectCornerBase = 0;
            if (rectList && !g_rectListGs && !ExpandRectList(vs, indexData, rectCorners, rectCornerBase))
            {
                Skip(SKIP_PRIMITIVE);
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
            rhi::Device::Upload indexUpload = AllocateUpload(indexBytes, 16);
            if (!indexUpload.cpu)
                return;
            memcpy(indexUpload.cpu, indexData.indices.data(), indexBytes);

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
                const uint8_t* uploaded;  // host copy (traces)
                uint32_t copyDwords;      // read from guest memory; the rest: made rectangle corners
                const VertexFetchBinding* binding;
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
                int64_t start = 0, end = size, made = 0;
                if (binding.indexedByVertexIndex && binding.minOffsetDwords <= binding.maxEndDwords)
                {
                    // The guest's vertices; made rectangle corners (rectCorners) after them.
                    uint32_t guestMax = rectCorners.empty() ? indexData.maxIndex : rectCornerBase - 1;
                    start = int64_t(indexData.minIndex) * binding.strideDwords + binding.minOffsetDwords;
                    end = int64_t(guestMax) * binding.strideDwords + binding.maxEndDwords;
                    if (!rectCorners.empty())
                        made = int64_t(indexData.maxIndex) * binding.strideDwords + binding.maxEndDwords;
                }
                start = std::clamp<int64_t>(start, 0, size);
                end = std::clamp<int64_t>(end, start, size);
                end = std::min<int64_t>(end, start + MAX_VERTEX_RANGE_BYTES / 4);
                uint32_t guestAddress = gpu::GuestFromGpuAddress(fetch.Address());
                if (uint64_t(fetch.Address() & 0x1FFFFFFF) + uint64_t(end) * 4 > PHYSICAL_SIZE)
                    continue;
                Range& range = ranges[rangeCount++];
                uint32_t total = uint32_t(std::max(end, std::min<int64_t>(made, start + MAX_VERTEX_RANGE_BYTES / 4)) - start);
                range = { fc, guestAddress, uint32_t(start), total, fetch.Endianness(), nullptr, uint32_t(end - start), &binding };
                vertexBytes += AlignUp64(range.dwords * 4, 16);
            }
            uint64_t vertexOffset = 0;
            if (vertexBytes)
            {
                rhi::Device::Upload vertexUpload = AllocateUpload(vertexBytes, 256);
                if (!vertexUpload.cpu)
                    return;
                vertexOffset = vertexUpload.offset;
                uint64_t cursor = 0;
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    Range& range = ranges[r];
                    const uint32_t* source = reinterpret_cast<const uint32_t*>(g_memory.Translate(range.guestAddress)) + range.startDwords;
                    uint32_t* destination = reinterpret_cast<uint32_t*>(vertexUpload.cpu + cursor);
                    range.uploaded = vertexUpload.cpu + cursor;
                    // One loop per swap mode, so each vectorizes (GpuSwap switches per dword).
                    switch (range.endian & 3)
                    {
                    case 0: memcpy(destination, source, size_t(range.copyDwords) * 4); break;
                    case 1: for (uint32_t i = 0; i < range.copyDwords; i++) destination[i] = xenos::GpuSwap(source[i], 1); break;
                    case 2: for (uint32_t i = 0; i < range.copyDwords; i++) destination[i] = __builtin_bswap32(source[i]); break;
                    default: for (uint32_t i = 0; i < range.copyDwords; i++) destination[i] = xenos::GpuSwap(source[i], 3); break;
                    }
                    if (range.dwords > range.copyDwords)
                    {
                        memset(destination + range.copyDwords, 0, size_t(range.dwords - range.copyDwords) * 4);
                        FillRectCorners(destination, range.startDwords, range.copyDwords, range.dwords, *range.binding, rectCorners, rectCornerBase);
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
            system.renderScale[0] = 1.0f / XScale(scale);
            system.renderScale[1] = 1.0f / scale;
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
            rhi::TextureView textureViews[32];
            rhi::SamplerDesc samplers[32];
            g_drawIconPalette = nullptr;
            g_drawIcon = 0;
            bool readsResolve = false;  // samples a render target (a screen-space pass, not a background)
            for (uint32_t fc = 0; fc < textureSlots; fc++)
            {
                textureViews[fc] = {};
                samplers[fc] = rhi::SamplerDesc{};
                if (!bindings[fc])
                    continue;
                xenos::TextureFetch fetch;
                for (uint32_t k = 0; k < 6; k++)
                    fetch.dword[k] = Reg(xenos::REG_SHADER_CONSTANT_FETCH_00_0 + fc * 6 + k);
                float hdScale;
                textureViews[fc] = GetTextureView(fetch, &hdScale);
                ResolveTarget* fetchedTarget = FindResolveTarget(fetch.BaseAddress());
                samplers[fc] = MakeSampler(fetch, bindings[fc], !fetchedTarget);
                if (hdScale != 1.0f)
                    AdjustSamplerForHd(samplers[fc], hdScale);
                readsResolve |= fetchedTarget != nullptr;
                bool fetchedScaled = fetchedTarget && fetchedTarget->Current().scale > 0.0f;
                system.texSizeScale[fc * 2] = fetchedScaled ? 1.0f / XScale(fetchedTarget->Current().scale) : hdScale;
                system.texSizeScale[fc * 2 + 1] = fetchedScaled ? 1.0f / fetchedTarget->Current().scale : hdScale;
                system.texSwizzles[fc] = fetch.Swizzle();
                if (trace)
                {
                    ResolveTarget* resolved = FindResolveTarget(fetch.BaseAddress());
                    Log("[gpu]   tf%u: base 0x%08X format %u %ux%u pitch %u tiled %u swizzle 0x%03X filter fetch %u/%u/%u/%u instr %u/%u/%u/%u -> 0x%X%s", fc, fetch.BaseAddress(), fetch.Format(),
                        fetch.Width(), fetch.Height(), fetch.Pitch(), uint32_t(fetch.Tiled()), fetch.Swizzle(),
                        fetch.MagFilter(), fetch.MinFilter(), fetch.MipFilter(), fetch.AnisoFilter(),
                        bindings[fc]->magFilter, bindings[fc]->minFilter, bindings[fc]->mipFilter, bindings[fc]->anisoFilter, uint32_t(samplers[fc].minFilter) | (uint32_t(samplers[fc].magFilter) << 1) | (uint32_t(samplers[fc].mipFilter) << 2) | (uint32_t(samplers[fc].anisotropy) << 4),
                        resolved ? (resolved->Current().packed ? " (resolve target, has packed depth)" : " (resolve target)") : "");
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
                        textureViews[fc] = { g_drawIconPalette };
                }
            }

            // Wide screens: the 2D of the screen back into the middle 16:9 area (widescreen.h).
            HudDraw hud = HudTransform(sceneDraw, pitch, msaa, ps ? ps->info.hash : 0, readsResolve);
            // Rectangle lists are D3D's clears (colour and depth, per tile): they cover what they
            // cover. Squeezed, they left the HUB's depth uncleared in strips (titan dolls cut).
            if (rectList)
                hud = HudDraw{};
            if (hud.video && colorTargets[0])
                ClearVideoBars(*colorTargets[0]);
            // The frontend's page through its camera: a frontend screen, for the menus' mouse.
            if (!g_pageNoted && !sceneDraw && vs && vs->info.hash == HUD_VS && uint32_t(targetWidth) == HUD_WIDTH)
            {
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    const VertexFetchBinding* position = nullptr;
                    for (const VertexFetchBinding& binding : vs->info.vertexFetches)
                        if (binding.fetchConstant == ranges[r].fetchConstant && !position)
                            position = &binding;
                    if (position && position->minOffsetDwords == 0 && position->strideDwords >= 3)
                        NotePage(reinterpret_cast<const float*>(ranges[r].uploaded), ranges[r].startDwords, ranges[r].dwords, position->strideDwords,
                                 indexData.minIndex, indexData.maxIndex);
                    break;
                }
            }
            // A button icon: where it is, for the clickable prompts.
            if (g_drawIcon && !sceneDraw && vs && vs->info.hash == HUD_VS)
            {
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    const VertexFetchBinding* position = nullptr;
                    for (const VertexFetchBinding& binding : vs->info.vertexFetches)
                        if (binding.fetchConstant == ranges[r].fetchConstant && !position)
                            position = &binding;
                    if (position && position->minOffsetDwords == 0 && position->strideDwords >= 3)
                        NoteIconSpot(reinterpret_cast<const float*>(ranges[r].uploaded), ranges[r].startDwords, ranges[r].dwords,
                                     position->strideDwords, indexData.minIndex, indexData.maxIndex, system, float(targetWidth),
                                     float(targetHeight), hud);
                    break;
                }
            }
            // The 2D's HUD_GS work folded into c4..c7 when the draw allows it (HudFoldDraw).
            static const bool hudFoldOn = []() { const char* v = getenv("CRASH_HUD_FOLD"); return !v || strcmp(v, "0") != 0; }();
            HudFold hudFold;
            bool hudFolded = false;
            if (hudFoldOn && hud.mode && !rectList && polygonal && vs->info.hash == HUD_VS)
            {
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    const VertexFetchBinding* position = nullptr;
                    for (const VertexFetchBinding& binding : vs->info.vertexFetches)
                        if (binding.fetchConstant == ranges[r].fetchConstant && !position)
                            position = &binding;
                    if (position && position->minOffsetDwords == 0 && position->strideDwords >= 3)
                        hudFolded = HudFoldDraw(reinterpret_cast<const float*>(ranges[r].uploaded), ranges[r].startDwords, ranges[r].dwords,
                                                position->strideDwords, indexData.indices, indexData.minIndex, indexData.maxIndex, topology,
                                                system, float(targetWidth), float(targetHeight), hud, hudFold);
                    break;
                }
                g_work.hudFolded += hudFolded;
                g_work.hudDraws++;
                static int s_traceLeft = []() { const char* v = getenv("CRASH_HUD_FOLD_TRACE"); return v ? atoi(v) : 0; }();
                if (!hudFolded && s_traceLeft > 0)
                {
                    s_traceLeft--;
                    Log("[gpu] 2D draw left to HUD_GS: %zu indices (%u..%u), topology %d, flags 0x%X, mode %u",
                        indexData.indices.size(), indexData.minIndex, indexData.maxIndex, int(topology), system.flags, hud.mode);
                }
            }

            ProfileMark(3);
            // Constant buffers.
            rhi::Device::Upload systemUpload = AllocateUpload(AlignUp64(sizeof(SystemConstants), 256), 256);
            rhi::Device::Upload vsConstUpload = AllocateUpload(4096, 256);
            rhi::Device::Upload psConstUpload = AllocateUpload(4096, 256);
            if (!systemUpload.cpu || !vsConstUpload.cpu || !psConstUpload.cpu)
                return;
            memcpy(systemUpload.cpu, &system, sizeof(system));
            for (int stage = 0; stage < 2; stage++)
            {
                uint32_t constReg = Reg(stage == 0 ? xenos::REG_SQ_VS_CONST : xenos::REG_SQ_PS_CONST);
                uint32_t base = constReg & 0x1FF;
                uint32_t* destination = reinterpret_cast<uint32_t*>(stage == 0 ? vsConstUpload.cpu : psConstUpload.cpu);
                const uint32_t* source = reinterpret_cast<const uint32_t*>(
                    g_memory.Translate(GPU_MMIO_BEGIN + (xenos::REG_SHADER_CONSTANT_000_X + base * 4) * 4));
                uint32_t available = base * 4 < 2048 ? std::min(1024u, 2048u - base * 4) : 0u;
                for (uint32_t i = 0; i < available; i++)
                    destination[i] = __builtin_bswap32(source[i]);
                for (uint32_t i = available; i < 1024; i++)
                    destination[i] = 0;
                if (stage == 0 && sceneDraw && ScenePassActive() && available >= 1024)
                    NoteSceneCamera(vs->info.hash, source, system.ndcScale[2], system.ndcOffset[2]);
                if (stage == 0 && hudFolded)
                {
                    if (available >= 32)
                        ApplyHudFold(reinterpret_cast<float*>(destination), hudFold, system);
                    else
                        hudFolded = false;
                }
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
            rhi::DrawPipelineKey key{};
            key.vs = vs->shader;
            key.ps = ps ? ps->shader : nullptr;
            key.gs = rectList ? g_rectListGs : nullptr;
            static const bool noHudGs = getenv("CRASH_HUD_NO_GS") != nullptr;  // debug: 2D placed per vertex
            if (hudFolded)
                hud.mode |= 32u;  // in c4..c7 already, not the vertex shader
            else if (hud.mode && !rectList && polygonal && g_hudGs && !noHudGs)
            {
                key.gs = g_hudGs;
                hud.mode |= 32u;  // the geometry shader does it, not the vertex shader
            }
            key.vsHash = vs->info.hash;
            key.psHash = ps ? ps->info.hash : 0;
            for (uint32_t i = 0; i < renderTargetCount; i++)
            {
                key.colorFormats[i] = colorTargets[i] ? colorTargets[i]->rtvFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
                key.blend[i] = Reg(xenos::BlendControlRegister(i));
            }
            key.colorCount = renderTargetCount;
            key.colorMask = colorMask;
            if (depthTarget)
            {
                key.depthFormat = depthTarget->rtvFormat;
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
                bool faceClockwise = ((modeControl >> 2) & 1) != 0;
                bool cullClockwise = faceClockwise ? cullFront : cullBack;
                bool cullCounterClockwise = faceClockwise ? cullBack : cullFront;
                key.cull = cullClockwise ? rhi::Cull::Front : (cullCounterClockwise ? rhi::Cull::Back : rhi::Cull::None);
                static const bool noCull = getenv("CRASH_GPU_NO_CULL") != nullptr;  // debug: no culling at all
                if (noCull)
                    key.cull = rhi::Cull::None;
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
                key.cull = rhi::Cull::None;
            }
            key.depthClip = !((clipControl >> 16) & 1);
            key.topology = topology;
            key.stripCut = indexData.hasCut;
            // [video] shader_compilation = "aggressive": a draw may be skipped while its pipeline
            // compiles only if nothing reads what it leaves behind after this frame: a draw into the
            // 3D scene (drawn again every frame) that writes colour only. Never clears, depth or
            // stencil writes (shadow volumes, depth read by later draws), nor the depth-alias clears.
            bool depthOrStencilWrite = depthTarget && ((depthControl & 1) || ((depthControl >> 2) & 1));
            bool deferrable = sceneDraw && renderTargetCount && !rectList && !aliasedColor && !depthOrStencilWrite;
            bool deferred = false;
            rhi::Pipeline* pipeline = g_rhi->GetDrawPipeline(key, deferrable, &deferred);
            if (!pipeline)
            {
                Skip(deferred ? SKIP_COMPILING : SKIP_PIPELINE);
                return;
            }
            rhi::Pipeline* aliasPipeline = nullptr;
            if (aliasedColor)
            {
                rhi::DrawPipelineKey aliasKey = key;
                aliasKey.ps = g_depthAliasPs;
                aliasKey.psHash = DEPTH_ALIAS_PS_KEY;
                aliasKey.colorFormats[0] = aliasedColor->rtvFormat;
                aliasKey.blend[0] = 0x00010001;
                aliasKey.colorCount = 1;
                aliasKey.colorMask = aliasedMask;
                aliasKey.depthFormat = 0;
                aliasKey.depthControl = 0;
                aliasKey.stencilMasks = 0;
                aliasKey.depthBias = 0;
                aliasKey.slopeBias = 0.0f;
                aliasPipeline = g_rhi->GetDrawPipeline(aliasKey);
            }
            if (trace)
            {
                rhi::Rect traced = GuestScissor(targetWidth, targetHeight);
                Log("[gpu]   drawn: %zu indices (range %u-%u), %u targets, depth %s%s, scissor (%d,%d)-(%d,%d), ndc scale %g %g offset %g %g, %u vertex ranges",
                    indexData.indices.size(), indexData.minIndex, indexData.maxIndex, renderTargetCount, depthTarget ? "yes" : "no",
                    aliasPipeline ? " (+ aliased color target)" : "",
                    int(traced.left), int(traced.top), int(traced.right), int(traced.bottom),
                    system.ndcScale[0], system.ndcScale[1], system.ndcOffset[0], system.ndcOffset[1], rangeCount);
                for (uint32_t r = 0; r < rangeCount; r++)
                {
                    const float* data = reinterpret_cast<const float*>(ranges[r].uploaded);
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
            // Record (the backend records only what changed since its last draw).
            rhi::DrawCall call;
            call.pipeline = pipeline;
            for (uint32_t i = 0; i < renderTargetCount; i++)
                call.colorTargets[i] = colorTargets[i] ? colorTargets[i]->texture : nullptr;
            call.colorCount = renderTargetCount;
            call.depthTarget = depthTarget ? depthTarget->texture : nullptr;
            call.textureCount = textureSlots;
            for (uint32_t i = 0; i < textureSlots; i++)
            {
                call.textures[i] = textureViews[i];
                call.samplers[i] = samplers[i];
            }
            call.systemConstants = systemUpload.offset;
            call.systemConstantBytes = uint32_t(sizeof(SystemConstants));
            call.vertexConstants = vsConstUpload.offset;
            call.pixelConstants = psConstUpload.offset;
            call.effects = g_effects;
            if (hud.mode)
            {
                call.effects.hud[0] = hud.scaleX;
                call.effects.hud[1] = hud.scaleY;
                call.effects.hud[2] = float(hud.mode);
                call.effects.hud[3] = hud.shift;
                call.effects.hudTarget[0] = float(targetWidth);
                call.effects.hudTarget[1] = float(targetHeight);
                call.effects.hudTarget[2] = float(HUD_WIDTH);
                call.effects.hudTarget[3] = float(HUD_HEIGHT);
            }
            if (sceneDraw && g_temporal.active)
            {
                // Sub-pixel jitter of the scene in NDC of this target (rectangle lists are clears).
                if (!rectList)
                {
                    call.effects.jitter[0] = 2.0f * g_temporal.jitterGuestX / float(targetWidth);
                    call.effects.jitter[1] = -2.0f * g_temporal.jitterGuestY / (float(targetHeight) * rowScale);
                }
                // Texture LOD bias of upscaling: mip selection for the output resolution.
                call.effects.textureLodScale = g_sceneScale / g_renderScale;
            }
            call.blendFactor[0] = RegF(xenos::REG_RB_BLEND_RED);
            call.blendFactor[1] = RegF(xenos::REG_RB_BLEND_RED + 1);
            call.blendFactor[2] = RegF(xenos::REG_RB_BLEND_RED + 2);
            call.blendFactor[3] = RegF(xenos::REG_RB_BLEND_RED + 3);
            call.stencilRef = Reg(xenos::REG_RB_STENCILREFMASK) & 0xFF;
            call.viewport = { 0.0f, 0.0f, hostW * XScale(scale), h * rowScale * scale };
            rhi::Rect scissor = GuestScissor(targetWidth, targetHeight);
            if (hud.mode && HudDrawIsOrtho(vs->info.hash))
                HudScissor(scissor);
            scissor.left = ScaledFloor(float(scissor.left * int32_t(samplesX)), XScale(scale));
            scissor.right = ScaledCeil(float(scissor.right * int32_t(samplesX)), XScale(scale));
            scissor.top = ScaledFloor(floorf(float(scissor.top) * rowScale), scale);
            scissor.bottom = std::min(ScaledCeil(ceilf(float(scissor.bottom) * rowScale), scale), int32_t(ScaledSize(RENDER_TARGET_HEIGHT, scale)));
            call.scissor = scissor;
            call.topology = topology;
            call.firstIndex = uint32_t(indexUpload.offset / 4);
            call.indexCount = uint32_t(indexData.indices.size());
            static const bool s_profileDraws = getenv("CRASH_GPU_PROFILE_DRAWS") != nullptr;
            if (s_profileDraws)
                ProfileLabel("%s draws EDRAM %d/%d vs %016llx ps %016llx", sceneDraw ? "scene" : "2D",
                             colorTargets[0] ? int(Reg(xenos::ColorInfoRegister(0)) & 0xFFF) : -1, depthTarget ? int(depthInfo & 0xFFF) : -1,
                             (unsigned long long)vs->info.hash, ps ? (unsigned long long)ps->info.hash : 0ull);
            else
                ProfileLabel("%s draws EDRAM %d/%d", sceneDraw ? "scene" : "2D", colorTargets[0] ? int(Reg(xenos::ColorInfoRegister(0)) & 0xFFF) : -1,
                             depthTarget ? int(depthInfo & 0xFFF) : -1);
            g_rhi->Draw(call);
            if (aliasPipeline)
            {
                call.pipeline = aliasPipeline;
                call.colorTargets[0] = aliasedColor->texture;
                call.colorCount = 1;
                call.depthTarget = nullptr;
                g_rhi->Draw(call);
                g_stats.aliasClears++;
            }
            ProfileMark(9);
        }

        // Presentation --------------------------------------------------------------------------

        void ResizeIfNeeded()
        {
            uint32_t width, height;
            window::GetClientSize(width, height);
            if (!width || !height || (width == g_backBufferWidth && height == g_backBufferHeight))
                return;
            g_rhi->Resize(width, height);
            g_rhi->BackBufferSize(g_backBufferWidth, g_backBufferHeight);
        }

        // The anti-aliasing targets: RGBA8 at the size of the front buffer, recreated when it changes.
        bool EnsureAaTargets(uint32_t width, uint32_t height)
        {
            if (g_aaInput && g_aaWidth == width && g_aaHeight == height)
                return true;
            DestroyTexture(g_aaInput);
            DestroyTexture(g_aaOutput);
            g_aaInput = CreateTexture(width, height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET, "anti-aliasing input");
            g_aaOutput = CreateTexture(width, height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET, "anti-aliasing output");
            if (!g_aaInput || !g_aaOutput)
            {
                Log("[gpu] could not create the anti-aliasing targets %ux%u", width, height);
                DestroyTexture(g_aaInput);
                DestroyTexture(g_aaOutput);
                return false;
            }
            g_aaWidth = width;
            g_aaHeight = height;
            return true;
        }

        // video.window = fullscreen: what the backend makes of it (Vulkan: the borderless window is
        // the fullscreen, nothing exclusive).
        void UpdateFullscreen()
        {
            static bool s_maybeFullscreen;
            bool wanted = window::Mode() == settings::WindowMode::Fullscreen;
            if (!wanted && !s_maybeFullscreen)
                return;
            s_maybeFullscreen = wanted;
            g_rhi->UpdateFullscreen(wanted, window::IsFocused());
        }

        void SaveScreenshot(rhi::Texture* backBuffer, const std::filesystem::path& path)
        {
            std::vector<uint8_t> pixels;
            uint32_t width, height;
            if (!g_rhi->ReadBack(backBuffer, pixels, width, height))
                return;
            for (size_t i = 0; i < pixels.size(); i += 4)
                pixels[i + 3] = 255;
            if (WritePng(path, pixels.data(), width, height, width * 4))
                Log("[gpu] screenshot saved: %s", path.string().c_str());
            else
                Log("[gpu] could not write %s", path.string().c_str());
        }

        bool CreatePassPipelines()
        {
            bool geometryShaders = g_rhi->SupportsGeometryShaders();
            if (geometryShaders)
            {
                g_rectListGs = g_rhi->CreateShader(rhi::Stage::Geometry, RECT_LIST_GS, "rect_list_gs", rhi::Layout::Draw);
                g_hudGs = g_rhi->CreateShader(rhi::Stage::Geometry, HUD_GS, "hud_gs", rhi::Layout::Draw);
                if (!g_hudGs)
                    Log("[gpu] the wide-screen HUD geometry shader did not compile: 2D is placed per vertex");
            }
            g_depthAliasPs = g_rhi->CreateShader(rhi::Stage::Pixel, DEPTH_ALIAS_PS, "depth_alias_ps", rhi::Layout::Draw);
            g_fxaaPipeline = CreateBlitPipeline("fxaa", FXAA_PS, DXGI_FORMAT_R8G8B8A8_UNORM);
            return (g_rectListGs || !geometryShaders) && g_depthAliasPs && g_fxaaPipeline && GetResolvePipeline(DXGI_FORMAT_R8G8B8A8_UNORM) &&
                   GetResolveDepthPackedPipeline() && GetPresentPipeline(g_rhi->BackBufferFormat()) && GetPresentPipeline(DXGI_FORMAT_R8G8B8A8_UNORM);
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
            int64_t start, end, frequency;
            start = int64_t(plat::PerfCounter());
            WaitIdle();

            const settings::Resolution& resolution = settings::ResolutionOf(wanted.aspect, wanted.renderHeight);
            float renderScale = float(resolution.height) / 720.0f;
            float aspect = float(resolution.width) / float(resolution.height);
            float stretch = aspect / widescreen::BASE_ASPECT;
            if (fabsf(stretch - 1.0f) < 0.002f)
                stretch = 1.0f;
            upscaler::Select(wanted.upscaler);
            bool temporal = upscaler::Active() != settings::Upscaler::Off;
            if (temporal && !g_temporal.motionPipeline && !CreateTemporalPipeline())
            {
                Log("[gpu] could not create the motion vector pass: temporal upscaling off");
                temporal = false;
            }
            float sceneScale = temporal ? renderScale * settings::UpscalerRatio(wanted.upscalerQuality) : renderScale;
            bool scaled = renderScale != 1.0f || sceneScale != 1.0f || stretch != 1.0f || hd::Active();
            float oldRender = g_renderScale, oldScene = g_sceneScale, oldStretch = g_stretch;
            g_renderScale = renderScale;
            g_sceneScale = sceneScale;
            g_stretch = stretch;
            widescreen::SetAspect(aspect);

            // Temporal upscaling: no history across the change; its targets follow the sizes.
            Temporal& t = g_temporal;
            t.active = temporal;
            t.lastEvaluated = UINT64_MAX;
            t.havePrevious = t.haveViewProj = false;
            if (!temporal)
            {
                DestroyTexture(t.motion);
                DestroyTexture(t.output);
                t.renderWidth = t.renderHeight = t.outputWidth = t.outputHeight = 0;
            }

            size_t targets = 0, surfaces = 0;
            if (renderScale != oldRender || sceneScale != oldScene || stretch != oldStretch)
            {
                for (auto& [key, target] : g_renderTargets)
                {
                    CreateRenderTargetResource(target);
                    targets++;
                }
                for (auto& [base, target] : g_resolveTargets)
                {
                    // Surface 0 is at the output scale, 1 at the scene's when they differ.
                    bool had[2] = { target.surfaces[0].texture != nullptr, target.surfaces[1].texture != nullptr };
                    bool packed[2] = { target.surfaces[0].packed != nullptr, target.surfaces[1].packed != nullptr };
                    for (ResolveSurface& surface : target.surfaces)
                    {
                        DestroyTexture(surface.texture);
                        DestroyTexture(surface.packed);
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
                    if (!target.Current().texture)
                        target.current = target.surfaces[0].texture ? 0 : 1;
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
                    shader->shader = nullptr;  // the backend keeps the old ones (and their pipelines)
                }
                Log("[gpu] shaders will be translated again (%s variant)", scaled ? "scaled" : "720p");
            }
            g_rhi->Submit();  // the clears of the new resources
            settings::RendererApplied(wanted.renderHeight, wanted.aspect, wanted.upscaler, wanted.upscalerQuality);

            end = int64_t(plat::PerfCounter());
            frequency = int64_t(plat::PerfFrequency());
            Log("[gpu] reconfigured: internal resolution %s (%.2fx), %s%s%s, scene %.3fx; %zu render targets and %zu resolve surfaces recreated in %.0f ms; aspect %.3f",
                resolution.name, renderScale, temporal ? upscaler::ActiveName().c_str() : "no upscaler",
                temporal ? " " : "", temporal ? settings::UpscalerQualityName(wanted.upscalerQuality) : "", sceneScale, targets, surfaces,
                double(end - start) * 1000.0 / double(frequency), aspect);
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
        widescreen::SetAspect(settings::RenderAspect());
        g_stretch = widescreen::Stretch();
        if (fabsf(g_stretch - 1.0f) < 0.002f)
            g_stretch = 1.0f;
        const settings::Resolution& startResolution = settings::ResolutionOf(settings::AtStartup().aspect, settings::AtStartup().renderHeight);
        if (g_renderScale != 1.0f || g_stretch != 1.0f)
            Log("[gpu] internal resolution %s (%.2fx the game's 1280x720, aspect %.3f: %.3fx as wide as 16:9)", startResolution.name, g_renderScale,
                widescreen::Aspect(), g_stretch);

        g_debugLayer = getenv("CRASH_GPU_DEBUG") != nullptr;
        // [video] gpu_safe_mode (CRASH_GPU_SAFE overrides it) and gpu_diagnostics (CRASH_GPU_DIAG).
        const settings::Values& startup = settings::AtStartup();
        const char* safeOverride = getenv("CRASH_GPU_SAFE");
        uint32_t safeMode = rhi::ParseSafeMode(safeOverride ? safeOverride : startup.gpuSafeMode);
        bool diagnostics = startup.gpuDiagnostics || getenv("CRASH_GPU_DIAG") != nullptr;
        if (diagnostics && !g_debugLayer)
        {
            // The validation layer copied into vulkan_layers\ next to the exe (players have no Vulkan
            // SDK): with it the diagnostics run also checks the renderer's use of Vulkan.
            std::filesystem::path layers = paths::ExeDir() / "vulkan_layers";
            std::error_code ignored;
            if (std::filesystem::exists(layers / "VkLayer_khronos_validation.json", ignored))
            {
                plat::SetEnvironment("VK_ADD_LAYER_PATH", plat::NarrowUtf8(layers.wstring()).c_str());
                g_debugLayer = true;
                Log("[gpu] diagnostics: validation layer from %s", layers.string().c_str());
            }
        }
        g_shaderCacheDir = vfs::UserDataDir() / "shadercache";
        std::error_code error;
        std::filesystem::create_directories(g_shaderCacheDir, error);
        g_dumpShaders = getenv("CRASH_DUMP_SHADERS") != nullptr;

        // Window and device (with its swap chain).
        window::SetMode(settings::Get().window);
        // A window of the picture's aspect, 720 lines (or the monitor's width, if narrower).
        uint32_t windowWidth = uint32_t(std::lround(float(WINDOW_HEIGHT) * widescreen::Aspect() / 2.0f)) * 2u;
        g_window = window::Create(windowWidth, WINDOW_HEIGHT, L"Crash of the Titans (CrashRecomp)");
        if (!g_window)
        {
            Log("[gpu] could not create the window");
            return false;
        }
        rhi::DeviceInfo info;
        info.window = g_window;
        info.width = windowWidth;
        info.height = WINDOW_HEIGHT;
        info.shaderCache = g_shaderCacheDir;
        info.debug = g_debugLayer;
        info.safeMode = safeMode;
        info.diagnostics = diagnostics;
        info.shaderCompilation = RhiShaderCompilation(startup.shaderCompilation);
        upscaler::VulkanExtensions(info.instanceExtensions, info.deviceExtensions);
        g_rhi = rhi::CreateVulkan(info);
        if (!g_rhi)
        {
            // The log says which part failed (driver, GPU features, dxcompiler.dll).
            Log("[gpu] no Vulkan device: the game cannot be shown");
            plat::ShowMessageBox("Crash of the Titans (CrashRecomp)",
                                 "O jogo precisa de uma placa de vídeo com Vulkan 1.3 (driver atualizado) e do dxcompiler.dll ao lado do "
                                 "executável do jogo.\n\n"
                                 "The game needs a Vulkan 1.3 GPU (an up-to-date driver) and dxcompiler.dll next to the game's exe.",
                                 plat::MessageKind::Error, g_window);
            plat::ExitProcess(3);
        }
        g_rhi->BackBufferSize(g_backBufferWidth, g_backBufferHeight);
        gpu::SetHostBlockCompression(g_rhi->SupportsBlockCompression());
        SetImplicitDerivatives(g_rhi->SupportsImplicitDerivatives());

        // Temporal upscaler: probed on this device; the scene scale depends on it.
        upscaler::Init(g_rhi, settings::AtStartup().upscaler, (vfs::UserDataDir() / "ngx").wstring());
        if (upscaler::Active() != settings::AtStartup().upscaler)
        {
            // It fell back (DLSS asked for where it can't run): the settings screen shows the one in
            // use, and settings.toml changes only if the player saves the options.
            settings::Values values = settings::Get();
            values.upscaler = upscaler::Active();
            settings::Set(values);
            settings::RendererApplied(settings::AtStartup().renderHeight, settings::AtStartup().aspect, upscaler::Active(),settings::AtStartup().upscalerQuality);
        }
        g_temporal.active = upscaler::Active() != settings::Upscaler::Off;
        g_sceneScale = g_temporal.active ? settings::SceneScale() : g_renderScale;
        // HD textures need the guest texel size in the shaders too (xe_tex_size_scale).
        hd::Init();
        g_renderScaled = g_renderScale != 1.0f || g_sceneScale != 1.0f || g_stretch != 1.0f || hd::Active();
        SetRenderScaled(g_renderScaled);
        if (g_temporal.active)
            Log("[gpu] %s %s: scene at %.3fx the game's 1280x720, output %.3fx", upscaler::ActiveName().c_str(),
                settings::UpscalerQualityName(settings::AtStartup().upscalerQuality), g_sceneScale, g_renderScale);
        watch::Init();

        if (!CreatePassPipelines())
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

        if (!smaa::Init(g_rhi))
            Log("[gpu] SMAA unavailable: video.antialiasing = smaa falls back to FXAA");
        if (!lighting::Init(g_rhi))
            Log("[gpu] lighting remaster unavailable");

        ParseScreenshotSettings();
        overlay::Init(g_rhi, g_rhi->BackBufferFormat(), g_screenshotDir);
        if (const char* trace = getenv("CRASH_GPU_TRACE"))
            g_traceDraws = uint32_t(strtoul(trace, nullptr, 10));
        if (const char* from = getenv("CRASH_GPU_TRACE_FROM"))
            g_traceFrom = strtoull(from, nullptr, 10);
        if (const char* swap = getenv("CRASH_GPU_TRACE_SWAP"))
            g_traceSwap = strtoull(swap, nullptr, 10);

        Log("[gpu] Vulkan renderer on %s%s; shader cache %s", g_rhi->AdapterName().c_str(), g_debugLayer ? " (validation layers)" : "",
            g_shaderCacheDir.string().c_str());
        int displayWidth, displayHeight, displayHz;
        if (plat::DesktopDisplayMode(displayWidth, displayHeight, displayHz))
            Log("[gpu] primary display: %dx%d at %d Hz", displayWidth, displayHeight, displayHz);
        // Both variants of every known game shader into the SPIR-V cache, off the critical path,
        // and the pipeline parts of the variant in use (asynchronous shader compilation).
        warmup::StartBackground(g_shaderCacheDir, g_rhi, g_renderScaled);
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
        // HLSL -> SPIR-V with DXC, as the Vulkan renderer compiles them.
        g_shaderCacheDir = vfs::UserDataDir() / "shadercache";
        std::error_code error;
        std::filesystem::create_directories(g_shaderCacheDir, error);
        g_dumpShaders = true;

        int failures = 0, total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (entry.path().extension() != ".out")
                continue;
            ShaderStage stage;
            std::vector<uint32_t> ucode;
            if (!warmup::ReadShaderContainer(entry.path(), stage, ucode))
            {
                Log("[test] %s: not a shader container", entry.path().filename().string().c_str());
                continue;
            }
            ShaderEntry shader;
            shader.info.stage = stage;
            shader.ucode = ucode;
            shader.info.hash = HashUcode(shader.info.stage, ucode.data(), uint32_t(ucode.size()));
            total++;
            bool ok = PrepareShader(&shader) != nullptr;
            failures += !ok;
            Log("[test] %-28s %s_%016llx %4u dwords, %3u instructions, %zu vfetch const(s), %zu texture(s)%s: %s",
                entry.path().filename().string().c_str(), stage == ShaderStage::Vertex ? "vs" : "ps", (unsigned long long)shader.info.hash, uint32_t(ucode.size()), shader.info.instructionCount,
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
            warmup::Record(stage, hash, ucode, dwordCount);
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
        g_stats.swaps++;
        g_profileFrame = g_stats.swaps % 4 == 0;
        ProfileLabel("swap");
        overlay::BeginFrame(g_stats.swaps);
        settings::Values options = settings::Get();
        g_effects.motionBlur = float(options.motionBlur) / 100.0f;
        g_effects.depthOfField = float(options.depthOfField) / 100.0f;
        g_effects.shadowSoftness = float(options.shadowSoftness) / 100.0f;
        // [graphics] shader_compilation applies at once; while the pipelines of earlier runs take
        // long to compile again at start (a new driver), a notice says so.
        {
            static settings::ShaderCompilation s_compileMode = settings::AtStartup().shaderCompilation;
            if (options.shaderCompilation != s_compileMode)
            {
                s_compileMode = options.shaderCompilation;
                g_rhi->SetShaderCompilation(RhiShaderCompilation(s_compileMode));
            }
            static const uint64_t s_start = plat::TickCountMs();
            static bool s_precompiling = true;
            uint32_t done = 0, total = 0;
            if (s_precompiling)
                g_rhi->PrecompileProgress(done, total);
            if (s_precompiling && done >= total)
                s_precompiling = false;
            else if (s_precompiling && plat::TickCountMs() - s_start > 3000 && g_stats.swaps % 15 == 0)
                overlay::Notice(settings::Tr({ "PREPARING SHADERS... ", "PREPARANDO SHADERS... ", "PREPARATION DES SHADERS... ",
                                               "SHADER WERDEN VORBEREITET... ", "PREPARANDO SHADERS... ", "PREPARAZIONE SHADER... ",
                                               "SHADERS VOORBEREIDEN... " },
                                             settings::UiLanguageOf(options.language)) +
                                std::to_string(done * 100 / std::max(total, 1u)) + "%");
        }
        // F8: the lighting remaster off / on (not saved), with a notice on screen.
        if (window::ConsumeLightingToggles() & 1)
        {
            bool on = lighting::ToggleBypass();
            bool any = on && (lighting::CurrentOptions().Any() || options.softShadows);
            const settings::UiText off = { "LIGHTING REMASTER: OFF (F8)", "ILUMINACAO REMASTER: DESLIGADA (F8)",
                                           "ECLAIRAGE REMASTER: DESACTIVE (F8)", "BELEUCHTUNG REMASTER: AUS (F8)",
                                           "ILUMINACION REMASTER: DESACTIVADA (F8)", "ILLUMINAZIONE REMASTER: DISATTIVATA (F8)",
                                           "VERLICHTING REMASTER: UIT (F8)" };
            const settings::UiText enabled = { "LIGHTING REMASTER: ON (F8)", "ILUMINACAO REMASTER: LIGADA (F8)",
                                               "ECLAIRAGE REMASTER: ACTIVE (F8)", "BELEUCHTUNG REMASTER: AN (F8)",
                                               "ILUMINACION REMASTER: ACTIVADA (F8)", "ILLUMINAZIONE REMASTER: ATTIVATA (F8)",
                                               "VERLICHTING REMASTER: AAN (F8)" };
            const settings::UiText nothing = { "LIGHTING REMASTER: NOTHING CHOSEN IN SETTINGS", "ILUMINACAO REMASTER: NADA ESCOLHIDO EM SETTINGS",
                                               "ECLAIRAGE REMASTER: RIEN CHOISI DANS SETTINGS", "BELEUCHTUNG REMASTER: NICHTS IN SETTINGS",
                                               "ILUMINACION REMASTER: NADA ELEGIDO EN SETTINGS", "ILLUMINAZIONE REMASTER: NULLA IN SETTINGS",
                                               "VERLICHTING REMASTER: NIETS IN SETTINGS" };
            overlay::Notice(settings::Tr(!on ? off : any ? enabled : nothing, settings::UiLanguageOf(options.language)));
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
        if (sourceSurface && !sourceSurface->texture)
            source = nullptr, sourceSurface = nullptr;
        // Anti-aliasing at the front buffer's own size (internal resolution), before it is scaled
        // to the window. The targets are (re)created before anything of this present is recorded.
        bool antiAliasing = false;
        if (source && !showRaw && options.antiAliasing != settings::AntiAliasing::Off)
        {
            uint32_t aaWidth = std::min(ScaledSize(width ? width : source->width, XScale(sourceSurface->scale)), sourceSurface->hostWidth);
            uint32_t aaHeight = std::min(ScaledSize(height ? height : source->height, sourceSurface->scale), sourceSurface->hostHeight);
            antiAliasing = aaWidth && aaHeight && EnsureAaTargets(aaWidth, aaHeight);
        }

        rhi::Texture* backBuffer = g_rhi->BeginPresent();
        rhi::Pipeline* presentPipeline = GetPresentPipeline(g_rhi->BackBufferFormat());
        // Keep the game's aspect ratio: the image is fitted into the window and centred, with
        // black bars at the sides or top and bottom, instead of being stretched to the window.
        float imageX = 0.0f, imageY = 0.0f, imageWidth = float(g_backBufferWidth), imageHeight = float(g_backBufferHeight);
        if (source && !showRaw)
        {
            float frameWidth = float(width ? width : source->width) * g_stretch, frameHeight = float(height ? height : source->height);
            float aspect = frameWidth > 0.0f && frameHeight > 0.0f ? frameWidth / frameHeight : 16.0f / 9.0f;
            if (imageWidth > imageHeight * aspect + 0.5f)
                imageWidth = std::round(imageHeight * aspect);
            else if (imageHeight > imageWidth / aspect + 0.5f)
                imageHeight = std::round(imageWidth / aspect);
            imageX = std::floor((float(g_backBufferWidth) - imageWidth) * 0.5f);
            imageY = std::floor((float(g_backBufferHeight) - imageHeight) * 0.5f);
        }
        bool bars = imageWidth < float(g_backBufferWidth) || imageHeight < float(g_backBufferHeight);
        // [video] gamma / saturation (live): only the present that draws into the back buffer.
        float gamma = float(options.gamma) / 100.0f, saturation = float(options.saturation) / 100.0f;
        float constants[PRESENT_CONSTANTS] = { 1.0f, 1.0f, imageWidth, imageHeight, 0.0f, 0.0f, imageX, imageY, gamma, saturation, 0.0f, 0.0f };
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
        rhi::PassDraw present;
        present.pipeline = presentPipeline;
        present.targets[0] = backBuffer;
        present.textureCount = 1;
        present.constants = constants;
        present.constantBytes = sizeof(constants);
        if (source && showRaw)
        {
            bool packed = sourceSurface->packed != nullptr;
            present.textures[0] = { packed ? sourceSurface->packed : sourceSurface->texture };
            constants[0] = constants[1] = 1.0f;
            flags |= packed ? 4 : 8;
        }
        else if (source)
        {
            present.textures[0] = { sourceSurface->texture };
            constants[0] = float(width ? width : source->width) / float(source->width);
            constants[1] = float(height ? height : source->height) / float(source->height);
            g_stats.presentedFrames++;
        }
        else
        {
            flags |= 2;
        }
        memcpy(&constants[4], &flags, 4);
        if (TracingSwap())
            Log("[gpu] present: front buffer 0x%08X %ux%u, source %s (%ux%u, surface %ux%u scale %.2f), flags %u, swizzle 0x%03X, uv scale %g %g, image %g,%g %gx%g, aa %d",
                frontBuffer, width, height, source ? "found" : "none", source ? source->width : 0, source ? source->height : 0,
                sourceSurface ? sourceSurface->hostWidth : 0, sourceSurface ? sourceSurface->hostHeight : 0, sourceSurface ? sourceSurface->scale : 0.0f,
                flags, swizzle, constants[0], constants[1], imageX, imageY, imageWidth, imageHeight, int(antiAliasing));
        if (TracingSwap() && sourceSurface)
        {
            Log("[gpu] present texture %p (current %u)", (void*)sourceSurface->texture, source->current);
            // CRASH_GPU_DUMP_PRESENT=1: the texture each traced present samples, as a PNG.
            static const bool dumpPresent = getenv("CRASH_GPU_DUMP_PRESENT") != nullptr;
            std::vector<uint8_t> pixels;
            uint32_t dumpWidth, dumpHeight;
            if (dumpPresent && g_rhi->ReadBack(sourceSurface->texture, pixels, dumpWidth, dumpHeight))
            {
                for (size_t i = 3; i < pixels.size(); i += 4)
                    pixels[i] = 255;
                char name[64];
                snprintf(name, sizeof(name), "present_source_%05llu.png", (unsigned long long)g_stats.swaps);
                WritePng(g_screenshotDir / name, pixels.data(), dumpWidth, dumpHeight, dumpWidth * 4);
            }
        }
        present.viewport = { imageX, imageY, imageWidth, imageHeight };
        present.scissor = { int32_t(imageX), int32_t(imageY), int32_t(imageX + imageWidth), int32_t(imageY + imageHeight) };
        float presentConstants[PRESENT_CONSTANTS];
        if (antiAliasing)
        {
            // 1:1 copy with the swizzle, then FXAA / SMAA, then the scaled present below.
            float copyConstants[PRESENT_CONSTANTS] = { constants[0], constants[1], float(g_aaWidth), float(g_aaHeight), constants[4], constants[5], 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f };
            rhi::PassDraw copy = present;
            copy.pipeline = GetPresentPipeline(DXGI_FORMAT_R8G8B8A8_UNORM);
            copy.targets[0] = g_aaInput;
            copy.constants = copyConstants;
            copy.viewport = { 0.0f, 0.0f, float(g_aaWidth), float(g_aaHeight) };
            copy.scissor = { 0, 0, int32_t(g_aaWidth), int32_t(g_aaHeight) };
            g_rhi->DrawPass(copy);
            if (options.antiAliasing == settings::AntiAliasing::Smaa && smaa::Ready())
            {
                smaa::Apply(g_aaInput, g_aaOutput, g_aaWidth, g_aaHeight, g_stats.swaps);
            }
            else
            {
                rhi::PassDraw fxaa = copy;
                fxaa.pipeline = g_fxaaPipeline;
                fxaa.targets[0] = g_aaOutput;
                fxaa.textures[0] = { g_aaInput };
                g_rhi->DrawPass(fxaa);
            }
            // The scaled present of the result: no swizzle, no red/blue swap.
            present.textures[0] = { g_aaOutput };
            float scaled[PRESENT_CONSTANTS] = { 1.0f, 1.0f, imageWidth, imageHeight, 0.0f, 0.0f, imageX, imageY, gamma, saturation, 0.0f, 0.0f };
            uint32_t identity = 0x688, noFlags = 0;
            memcpy(&scaled[4], &noFlags, 4);
            memcpy(&scaled[5], &identity, 4);
            memcpy(presentConstants, scaled, sizeof(scaled));
            present.constants = presentConstants;
        }
        if (bars)
        {
            const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_rhi->ClearColor(backBuffer, black, nullptr);
        }
        bool probing = ProbingSwap(g_stats.swaps);
        if (probing && sourceSurface)
        {
            char tag[96];
            snprintf(tag, sizeof(tag), "swap %llu present source 0x%08X", (unsigned long long)g_stats.swaps, frontBuffer);
            g_rhi->Probe(present.textures[0].texture, tag);
        }
        g_rhi->DrawPass(present);
        if (probing)
            g_rhi->Probe(backBuffer, "swap " + std::to_string(g_stats.swaps) + " back buffer");
        if (TracingSwap() && getenv("CRASH_GPU_DUMP_PRESENT"))
        {
            std::vector<uint8_t> pixels;
            uint32_t dumpWidth, dumpHeight;
            if (g_rhi->ReadBack(backBuffer, pixels, dumpWidth, dumpHeight))
            {
                for (size_t i = 3; i < pixels.size(); i += 4)
                    pixels[i] = 255;
                char name[64];
                snprintf(name, sizeof(name), "present_after_%05llu.png", (unsigned long long)g_stats.swaps);
                WritePng(g_screenshotDir / name, pixels.data(), dumpWidth, dumpHeight, dumpWidth * 4);
            }
        }

        // The button icons of this frame, for clicks on the prompts.
        settings::prompts::PublishIconSpots(g_iconSpots);
        g_iconSpots.clear();
        // A frontend screen, for the menus' mouse.
        if (g_pageNoted)
            input::menu_mouse::NoteFrontendPage();
        g_pageNoted = false;
        // Drawn over the image at the window's resolution, through the overlay's bitmaps.
        overlay::BeginPresent(backBuffer);
        // Our menus and texts lay out in the window, or in the game's own 4:3 in the picture, where
        // its 2D is (widescreen::OverlayArea; the mouse maps the same, input::pc::CursorInClient).
        float areaX, areaY, areaWidth, areaHeight;
        widescreen::OverlayArea(g_backBufferWidth, g_backBufferHeight, areaX, areaY, areaWidth, areaHeight);
        const uint32_t overlayWidth = uint32_t(areaWidth), overlayHeight = uint32_t(areaHeight);
        overlay::SetOrigin(int(areaX), int(areaY));
        // Subtitles (subtitles/subtitle_draw.h): part of the image, so screenshots have them.
        subtitles::Draw(overlayWidth, overlayHeight);
        // The "hold to skip" prompt of the cutscenes and videos (skip_cinematics.h).
        cutscenes::Draw(overlayWidth, overlayHeight);
        // The skin challenges' counters in a level (challenge_counters.h).
        settings::challenges::Draw(overlayWidth, overlayHeight);
        // The on-screen touch controls (Android, touch_controls.h): the whole screen.
        overlay::SetOrigin(0, 0);
        touch_controls::Draw(g_backBufferWidth, g_backBufferHeight);
        overlay::SetOrigin(int(areaX), int(areaY));

        // The name entry's keyboard and the slot screen's DELETE (save_screens.h), then the settings
        // screen (settings_menu.h).
        settings::save_screens::Draw(overlayWidth, overlayHeight);
        settings::menu::Draw(overlayWidth, overlayHeight);
        mp::menu::Draw(overlayWidth, overlayHeight);  // MULTIPLAYER (online/mp_menu.h)
        dev::cheat_menu::Draw(overlayWidth, overlayHeight);  // the cheat menu (pause + code)
        achievements::Draw(overlayWidth, overlayHeight);  // achievements: hint, screen, popup
        dev::console::Draw(overlayWidth, overlayHeight);  // the developer console
        overlay::SetOrigin(0, 0);

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
            SaveScreenshot(backBuffer, g_screenshotDir / screenshotName);
            overlay::NoteScreenshot(frame);
            screenshot = false;
        }
        overlay::Draw(g_backBufferWidth, g_backBufferHeight);
        if (screenshot)
        {
            SaveScreenshot(backBuffer, g_screenshotDir / screenshotName);
            overlay::NoteScreenshot(frame);
        }

        g_rhi->Present(options.vsync);

        // Frame pacing stats: host time between presents, logged every 300 frames.
        {
            static int64_t s_frequency, s_last;
            static double s_sum, s_max;
            static uint32_t s_count, s_hitches;
            static rhi::Timing s_timing;
            rhi::Timing timing = g_rhi->TakeTiming();
            s_timing.frameWaitTicks += timing.frameWaitTicks;
            s_timing.submitTicks += timing.submitTicks;
            s_timing.ringWaitTicks += timing.ringWaitTicks;
            s_timing.presentTicks = std::max(s_timing.presentTicks, timing.presentTicks);
            int64_t now = int64_t(plat::PerfCounter());
            if (!s_frequency)
                s_frequency = int64_t(plat::PerfFrequency());
            if (s_last)
            {
                double ms = double(now - s_last) * 1000.0 / double(s_frequency);
                s_sum += ms;
                s_max = std::max(s_max, ms);
                s_count++;
                double toMs = 1000.0 / double(s_frequency);
                double pipelineMs = double(timing.pipelineTicks) * toMs, shaderMs = double(timing.shaderTicks) * toMs;
                // Hitches over 50 ms and what compiled in them (first use of a shader or pipeline).
                if (ms > 50.0)
                {
                    s_hitches++;
                    static uint32_t s_loggedHitches;
                    if (s_loggedHitches++ < 200)
                        Log("[gpu] hitch: frame %llu took %.1f ms; compiled %u pipelines (%.1f ms), %u shaders (%.1f ms)", (unsigned long long)g_frame, ms,
                            timing.pipelines, pipelineMs, timing.shaders, shaderMs);
                }
                // CRASH_COMPILE_LOG=1: every frame's time and compiles in compiles.csv (screenshot folder).
                static FILE* s_frameLog = [] {
                    FILE* file = getenv("CRASH_COMPILE_LOG") ? plat::OpenCFile(g_screenshotDir / "compiles.csv", "w") : nullptr;
                    if (file)
                        fprintf(file, "frame,ms,pipelines,pipeline_ms,shaders,shader_ms\n");
                    return file;
                }();
                if (s_frameLog)
                {
                    fprintf(s_frameLog, "%llu,%.3f,%u,%.3f,%u,%.3f\n", (unsigned long long)g_frame, ms, timing.pipelines, pipelineMs, timing.shaders, shaderMs);
                    fflush(s_frameLog);  // test runs end by a kill
                }
                if (s_count == 300)
                {
                    Log("[gpu] present pacing: %u frames, avg %.2f ms, max %.2f ms, %u over 50 ms; waiting for the GPU %.2f ms/frame, submit %.2f ms/frame, longest Present %.2f ms, ring waits %.2f ms/frame; window %s",
                        s_count, s_sum / s_count, s_max, s_hitches, double(s_timing.frameWaitTicks) * 1000.0 / double(s_frequency) / s_count,
                        double(s_timing.submitTicks) * 1000.0 / double(s_frequency) / s_count, double(s_timing.presentTicks) * 1000.0 / double(s_frequency),
                        double(s_timing.ringWaitTicks) * 1000.0 / double(s_frequency) / s_count,
                        window::IsFocused() ? "focused" : "not focused");
                    s_timing = {};
                    std::string profile;
                    for (int i = 0; i < 10; i++)
                    {
                        char text[64];
                        snprintf(text, sizeof(text), "%s%s %.2f", i ? ", " : "", PROFILE_NAMES[i], double(g_profileTicks[i]) * 4.0 * 1000.0 / double(s_frequency) / 300.0);
                        profile += text;
                        g_profileTicks[i] = 0;
                    }
                    Log("[gpu] CP thread ms/frame: %s", profile.c_str());
                    static uint64_t s_faults;
                    uint64_t faults = watch::Faults();
                    Log("[gpu] CP work/frame: %.0f draws, %.0f indices, %.0f texture lookups, %.0f textures hashed (%.2f MB), %.0f unchanged by write watch, %.1f protected, %.1f write faults, %.1f texture uploads, %.2f MB of PM4, %.1f of %.1f 2D draws without HUD_GS",
                        g_work.draws / 300.0, g_work.indices / 300.0, g_work.textureLookups / 300.0, g_work.textureChecks / 300.0,
                        g_work.textureBytes / 300.0 / 1048576.0, g_work.textureWatched / 300.0, g_work.textureProtects / 300.0,
                        double(faults - s_faults) / 300.0, g_work.textureUploads / 300.0, double(gpu::TakeCommandDwords()) * 4.0 / 300.0 / 1048576.0,
                        g_work.hudFolded / 300.0, g_work.hudDraws / 300.0);
                    s_faults = faults;
                    g_work = {};
                    s_sum = s_max = 0.0;
                    s_count = s_hitches = 0;
                }
            }
            s_last = now;
        }
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
            g_rhi->LeaveFullscreen();
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
            "%zu textures (%llu hd), %zu render targets, %zu resolve targets, %zu shaders",
            (unsigned long long)g_stats.draws, (unsigned long long)g_stats.drawsSkipped, (unsigned long long)g_stats.resolves,
            (unsigned long long)g_stats.clears, (unsigned long long)g_stats.aliasClears, (unsigned long long)g_stats.swaps, (unsigned long long)g_stats.presentedFrames,
            (unsigned long long)g_stats.textureUploads, g_textures.size(), (unsigned long long)g_stats.hdTextures, g_renderTargets.size(), g_resolveTargets.size(), g_shaders.size());
        g_rhi->LogStats();
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
