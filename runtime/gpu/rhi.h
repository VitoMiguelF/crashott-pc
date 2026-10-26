#pragma once

// Rendering hardware interface: what the Xenos renderer (xenos_renderer.cpp) and its passes (smaa,
// lighting, overlay) need from a graphics API, implemented by rhi_vulkan.cpp (Vulkan 1.3). It kept
// the shape it had when Direct3D 12 was a second backend (until 2026-10-23), so another API (Metal,
// a native Linux build's WSI) only needs a new Device.
//
// The model is the renderer's own, kept small on purpose:
// - One command stream on one queue, recorded and submitted by the backend (Submit on demand,
//   Present at the end of a frame). Resource states (Vulkan layouts and access) are
//   tracked per texture and the barriers recorded on use: callers never write barriers.
// - An upload ring (host-visible, ~256 MB): vertex data, indices, constants and texture uploads are
//   written there by the CPU and referenced by byte offset. Space is reclaimed by fence.
// - Xenos draws: a fixed binding model shared with the shader translator (shader_translator.h):
//   b0 system constants, b1/b2 float constants, b3 effect constants (graphics_options.h), t0-t31
//   textures, s0-s31 samplers, t0 space1 the upload ring as a raw buffer (vertex data).
// - Passes: full-screen triangles with up to 4 targets, b0 constants, up to 8 textures (or one
//   structured buffer at t0) and up to 2 static samplers (s0, s1): resolves, present, FXAA, SMAA,
//   motion vectors, the lighting remaster, the overlay.
// - Formats are DXGI_FORMAT values (only the enum, from <dxgiformat.h>); the Vulkan backend maps them.
// - Shaders are HLSL compiled to SPIR-V by DXC, cached on disk by a hash of the source
//   (userdata\shadercache\*.spv).
//
// Everything runs on the command processor thread (renderer.h), except Create (Init).

#include "graphics_options.h"

#include "dxgi_format.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gpu::rhi
{
    struct Rect
    {
        int32_t left = 0, top = 0, right = 0, bottom = 0;
    };

    struct Viewport
    {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        float minDepth = 0.0f, maxDepth = 1.0f;
    };

    // Textures --------------------------------------------------------------------------------

    enum TextureUsage : uint32_t
    {
        USAGE_SAMPLED = 1,
        USAGE_RENDER_TARGET = 2,
        USAGE_DEPTH_STENCIL = 4,
        USAGE_STORAGE = 8,  // unordered access (the upscalers' output)
        // Every texture can be a copy source and destination.
    };

    struct TextureDesc
    {
        uint32_t width = 1, height = 1, levels = 1;
        // The format shaders sample. Depth: DXGI_FORMAT_D24_UNORM_S8_UINT or D32_FLOAT_S8X24_UINT
        // (sampled through View::Depth / View::Stencil).
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        // The render target view's format when it differs (R8G8B8A8_UNORM_SRGB for gamma targets).
        DXGI_FORMAT renderFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t usage = USAGE_SAMPLED;
        const char* name = "";

        DXGI_FORMAT RenderFormat() const { return renderFormat != DXGI_FORMAT_UNKNOWN ? renderFormat : format; }
    };

    class Texture
    {
    public:
        virtual ~Texture() = default;
        TextureDesc desc;
    };

    enum class View : uint8_t
    {
        Default,
        Depth,    // depth of a depth/stencil texture, as a float in .r
        Stencil,  // stencil of a depth/stencil texture, as an uint in .x (HLSL: XE_STENCIL)
    };

    struct TextureView
    {
        Texture* texture = nullptr;  // null: a transparent black texture
        View view = View::Default;
    };

    // Host-visible buffer, mapped for its whole life (the overlay's images).
    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        uint8_t* cpu = nullptr;
        uint64_t size = 0;
    };

    // Layout of one texture level in upload memory (D3D12-style copyable footprint rules: rows of
    // blocks, the row pitch aligned to 256 bytes, the level to UPLOAD_PLACEMENT_ALIGNMENT).
    constexpr uint64_t UPLOAD_PLACEMENT_ALIGNMENT = 512;
    struct Footprint
    {
        uint32_t rowBytes = 0;   // bytes of one row of blocks
        uint32_t rowPitch = 0;   // aligned
        uint32_t rows = 0;       // rows of blocks
        uint64_t size = 0;       // rowPitch * rows
    };
    Footprint LevelFootprint(DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t level);
    // Block size (1x1 for uncompressed) and bytes per block.
    void FormatBlock(DXGI_FORMAT format, uint32_t& blockWidth, uint32_t& blockHeight, uint32_t& bytesPerBlock);

    // Samplers -------------------------------------------------------------------------------

    enum class Filter : uint8_t { Point, Linear };
    enum class Address : uint8_t { Wrap, Mirror, Clamp, MirrorOnce, Border };
    enum class BorderColor : uint8_t { TransparentBlack, OpaqueBlack, OpaqueWhite };

    // Compared and hashed as bytes: keep it free of padding.
    struct SamplerDesc
    {
        Filter minFilter = Filter::Point, magFilter = Filter::Point, mipFilter = Filter::Point;
        uint8_t anisotropy = 1;  // > 1: anisotropic (the filters are then linear)
        Address addressU = Address::Clamp, addressV = Address::Clamp, addressW = Address::Clamp;
        BorderColor border = BorderColor::TransparentBlack;
        float minLod = 0.0f, maxLod = 3.402823466e+38f;
    };
    static_assert(sizeof(SamplerDesc) == 16);

    // Shaders and pipelines --------------------------------------------------------------------

    enum class Stage : uint8_t { Vertex, Pixel, Geometry };
    // Binding model the shader is compiled for (see the top of this file).
    enum class Layout : uint8_t { Draw, Pass };

    class Shader
    {
    public:
        virtual ~Shader() = default;
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;
    };

    enum class Topology : uint8_t { PointList, LineList, LineStrip, TriangleList, TriangleStrip };
    enum class Cull : uint8_t { None, Front, Back };

    // A Xenos draw's pipeline. Blend, depth and stencil state are the raw Xenos registers, which
    // each backend translates. Hashed as bytes: value-initialize it ({}).
    struct DrawPipelineKey
    {
        Shader* vs = nullptr;
        Shader* ps = nullptr;    // null: depth only
        Shader* gs = nullptr;    // rectangle lists
        uint64_t vsHash = 0, psHash = 0;
        uint32_t colorFormats[4] = {};  // DXGI render view formats
        uint32_t depthFormat = 0;       // DXGI depth format, 0 = none
        uint32_t colorCount = 0;
        uint32_t blend[4] = {};         // RB_BLENDCONTROL0-3
        uint32_t colorMask = 0;         // 4 bits per target
        uint32_t depthControl = 0;      // RB_DEPTHCONTROL
        uint32_t stencilMasks = 0;      // RB_STENCILREFMASK & 0xFFFF00 (read mask << 8, write mask << 16)
        int32_t depthBias = 0;          // in units of 2^-24 (24-bit depth)
        float slopeBias = 0.0f;
        Cull cull = Cull::None;
        Topology topology = Topology::TriangleList;
        uint8_t depthClip = 1;
        uint8_t stripCut = 0;           // primitive restart (0xFFFFFFFF)
        // Backend use: every colorFormats entry is an attachment, also where colorMask is 0 (a draw
        // recorded inside a rendering instance with more attachments than it writes).
        uint8_t keepFormats = 0;
    };

    struct DrawCall
    {
        Pipeline* pipeline = nullptr;
        Texture* colorTargets[4] = {};  // null entries are allowed below colorCount
        uint32_t colorCount = 0;
        Texture* depthTarget = nullptr;
        Viewport viewport;
        Rect scissor;
        float blendFactor[4] = {};
        uint32_t stencilRef = 0;
        // Upload ring offsets of the constant buffers: system (b0), vertex (b1, 4 KB), pixel (b2, 4 KB).
        uint64_t systemConstants = 0, vertexConstants = 0, pixelConstants = 0;
        uint32_t systemConstantBytes = 0;
        graphics::EffectConstants effects;  // b3
        uint32_t textureCount = 0;          // slots used (t/s 0..textureCount-1)
        TextureView textures[32];
        SamplerDesc samplers[32];
        Topology topology = Topology::TriangleList;
        uint32_t firstIndex = 0;            // in 32-bit indices from the start of the upload ring
        uint32_t indexCount = 0;
    };

    enum class Blend : uint8_t
    {
        Opaque,
        Alpha,  // color = src * src.a + dst * (1 - src.a), alpha kept (the overlay)
    };

    struct PassPipelineDesc
    {
        std::string name;
        std::string vs;  // HLSL; empty = a full-screen triangle (SV_VertexID 0-2)
        std::string ps;
        DXGI_FORMAT targets[4] = {};
        uint32_t targetCount = 1;
        Blend blend = Blend::Opaque;
        uint32_t textureCount = 0;     // t0..textureCount-1
        bool bufferInput = false;      // t0 is a StructuredBuffer<uint> (PassDraw::buffer) instead
        uint32_t constantBytes = 0;    // b0 (0 = none)
        SamplerDesc samplers[2];       // s0, s1
        uint32_t samplerCount = 0;
    };

    struct PassDraw
    {
        Pipeline* pipeline = nullptr;
        Texture* targets[4] = {};      // drawn through their render view format
        uint32_t targetCount = 1;
        Viewport viewport;
        Rect scissor;
        TextureView textures[8];
        uint32_t textureCount = 0;
        Buffer* buffer = nullptr;      // bufferInput pipelines
        uint64_t bufferOffset = 0, bufferBytes = 0;
        const void* constants = nullptr;
        uint32_t constantBytes = 0;
        uint32_t vertexCount = 3;
    };

    // Native access (the upscalers, which call vendor SDKs on the backend's own objects).
    enum class NativeState : uint8_t { ShaderRead, UnorderedAccess, CopySource, CopyDest, RenderTarget };
    struct NativeHandles
    {
        void* device = nullptr;       // VkDevice
        void* queue = nullptr;        // VkQueue
        void* commandList = nullptr;  // VkCommandBuffer (the one being recorded)
        void* physicalDevice = nullptr;  // VkPhysicalDevice
        void* instance = nullptr;        // VkInstance
        void* getInstanceProcAddr = nullptr;  // PFN_vkGetInstanceProcAddr
        void* getDeviceProcAddr = nullptr;    // PFN_vkGetDeviceProcAddr
    };

    struct Timing
    {
        uint64_t frameWaitTicks = 0;  // QPC ticks Present spent waiting for old frames
        uint64_t submitTicks = 0;     // ... submitting
        uint64_t presentTicks = 0;    // longest present call
        uint64_t ringWaitTicks = 0;   // ... waiting for upload ring space
        // Compiles on the recording thread (the hitches of first use): driver pipelines and
        // HLSL -> SPIR-V shaders not in the cache, their count and QPC ticks.
        uint32_t pipelines = 0, shaders = 0;
        uint64_t pipelineTicks = 0, shaderTicks = 0;
    };

    // GPU safe mode ([video] gpu_safe_mode, CRASH_GPU_SAFE): conservative paths, one bit each, for
    // drivers that show black frames or tiles. ParseSafeMode reads "on"/"all", "off" or a list of
    // the names below.
    enum SafeMode : uint32_t
    {
        SAFE_BARRIERS = 1,     // "barriers": every barrier waits for all earlier work, all memory
        SAFE_INSTANCES = 2,    // "instances": no draw recorded inside a larger rendering instance
        SAFE_SERIAL = 4,       // "serial": the CPU waits for each frame's GPU work (no frame overlap)
        SAFE_MEMORY = 8,       // "memory": a memory allocation per image (no sub-allocation)
        SAFE_DEPTH = 16,       // "depth": D32S8 instead of D24S8
        SAFE_PRESENT = 32,     // "present": no immediate present mode; keep swap chain image contents
        SAFE_CACHE = 64,       // "cache": ignore the pipeline cache on disk
        SAFE_ALL = 127,
    };
    uint32_t ParseSafeMode(const std::string& text);
    std::string SafeModeNames(uint32_t mask);

    // How draw pipelines are compiled ([video] shader_compilation, CRASH_SHADER_COMPILE).
    // - Sync ("sync"): on the recording thread when a draw first needs one (the old behaviour).
    // - Async ("async", the default): compile workers; with VK_EXT_graphics_pipeline_library a new
    //   pipeline is fast-linked from per-shader libraries (well under a millisecond) and an optimized
    //   one replaces it once a worker has linked it; the pipelines of earlier runs are compiled by
    //   the workers at start. Without the extension, only the start-up compiles are in the background.
    // - Aggressive ("aggressive"): async, and a draw that can wait (DrawPipelineKey: deferrable) is
    //   skipped while its pipeline compiles.
    enum class ShaderCompilation : uint8_t { Sync, Async, Aggressive };
    ShaderCompilation ParseShaderCompilation(const std::string& text);  // unknown = Async

    struct DeviceInfo
    {
        void* window = nullptr;  // the native window (gpu/window.h makes the surface)
        uint32_t width = 1280, height = 720;
        std::filesystem::path shaderCache;  // userdata\shadercache (empty: no disk cache)
        bool debug = false;                 // Vulkan validation layers (+ synchronization validation)
        uint32_t safeMode = 0;              // SafeMode bits
        bool diagnostics = false;           // [video] gpu_diagnostics: the device's features, limits and formats in the log
        ShaderCompilation shaderCompilation = ShaderCompilation::Async;
        // Vulkan extensions a vendor SDK asks for (DLSS's NGX), enabled when the driver has them.
        std::vector<std::string> instanceExtensions, deviceExtensions;
    };

    class Device
    {
    public:
        virtual ~Device() = default;
        virtual const std::string& AdapterName() const = 0;

        // Submission.
        virtual void Submit() = 0;
        virtual void WaitIdle() = 0;

        // Upload ring: cpu = null if it can't hold `size` bytes.
        struct Upload
        {
            uint8_t* cpu = nullptr;
            uint64_t offset = 0;
        };
        virtual Upload AllocateUpload(uint64_t size, uint64_t alignment) = 0;
        // Between true and false (nested), the uploads are for a draw not recorded yet: a
        // submission in between (the ring full) must not free them (rhi_ring.h).
        virtual void HoldUploads(bool hold) = 0;

        // Resources. Destroy* releases them once the GPU is done with them.
        virtual Texture* CreateTexture(const TextureDesc& desc) = 0;  // contents undefined (render targets: cleared to 0)
        virtual void DestroyTexture(Texture* texture) = 0;
        virtual Texture* NullTexture() = 0;
        virtual Buffer* CreateBuffer(uint64_t size) = 0;
        virtual void DestroyBuffer(Buffer* buffer) = 0;

        // Copies one level from the upload ring (LevelFootprint layout at `offset`).
        virtual void CopyUploadToTexture(Texture* texture, uint32_t level, uint64_t offset, uint32_t rowPitch) = 0;
        // Copies `source` (null: the whole texture) to the same position in `destination`.
        virtual void CopyTexture(Texture* destination, Texture* source, const Rect* region) = 0;
        virtual void ClearColor(Texture* texture, const float color[4], const Rect* rect) = 0;
        virtual void ClearDepthStencil(Texture* texture, float depth, uint8_t stencil, const Rect* rect) = 0;

        // Shaders: null (logged) if it doesn't compile.
        virtual Shader* CreateShader(Stage stage, const std::string& hlsl, const char* name, Layout layout) = 0;
        // Cached; null on failure. `deferrable`: the draw may be skipped while its pipeline compiles
        // (ShaderCompilation::Aggressive only); `deferred` is then set, and null returned.
        virtual Pipeline* GetDrawPipeline(const DrawPipelineKey& key, bool deferrable = false, bool* deferred = nullptr) = 0;
        // Pipelines of earlier runs still compiling in the background, of those queued at start.
        virtual void PrecompileProgress(uint32_t& done, uint32_t& total) = 0;
        // Changes the mode while running (the settings screen); the start-up compiles only run when
        // the device is created in an asynchronous mode.
        virtual void SetShaderCompilation(ShaderCompilation mode) = 0;
        // A draw shader (Layout::Draw) likely to be used: with graphics pipeline libraries, its part
        // is compiled by a worker ahead of use. Any thread (the shader warm-up's).
        virtual void PrewarmShader(Stage stage, const std::string& hlsl) = 0;
        virtual Pipeline* CreatePassPipeline(const PassPipelineDesc& desc) = 0;

        virtual void Draw(const DrawCall& draw) = 0;
        virtual void DrawPass(const PassDraw& pass) = 0;
        // BC1-5 textures can be sampled (false on Mali: texture_decode decodes them on the CPU).
        virtual bool SupportsBlockCompression() const { return true; }
        // Geometry shaders usable (false on Mali: the renderer's rectangle lists and wide-screen 2D
        // then go without them).
        virtual bool SupportsGeometryShaders() const { return true; }

        // Presentation. BeginPresent returns the back buffer (DXGI BackBufferFormat()) to draw the
        // frame into; Present submits and shows it, keeping at most two frames in flight.
        virtual DXGI_FORMAT BackBufferFormat() const = 0;
        virtual void BackBufferSize(uint32_t& width, uint32_t& height) const = 0;
        virtual bool Resize(uint32_t width, uint32_t height) = 0;
        virtual Texture* BeginPresent() = 0;
        virtual void Present(bool vsync) = 0;
        // video.window = fullscreen (the borderless window is the fullscreen: no exclusive mode).
        virtual void UpdateFullscreen(bool wanted, bool focused) = 0;
        virtual void LeaveFullscreen() = 0;
        // Reads `texture` (RGBA8 or BGRA8) back as RGBA8 rows; waits for the GPU.
        virtual bool ReadBack(Texture* texture, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) = 0;
        // Debug (CRASH_GPU_PROBE): an 8x8 grid of texels of `texture` (RGBA8 or BGRA8) copied at
        // this point of the command stream, logged with `tag` once the GPU is past it (no wait).
        virtual void Probe(Texture*, const std::string&) {}
        // Debug (CRASH_GPU_PROFILE=<first>-<last>, frames): GPU time of each rendering instance,
        // pass and copy of those frames, by the renderer's label and the operation, logged as a
        // table ([gpu profile] lines) once the GPU is past them.
        virtual bool Profiling() const { return false; }
        virtual void ProfileLabel(const char*) {}

        // GPU timestamps (the lighting remaster's timing): write, resolve after the work, read once
        // the frame that resolved them is done.
        virtual void WriteTimestamp(uint32_t index) = 0;  // index < 64
        virtual void ResolveTimestamps(uint32_t first, uint32_t count) = 0;
        virtual bool ReadTimestamps(uint32_t first, uint32_t count, uint64_t* ticks) = 0;
        virtual double TimestampFrequency() const = 0;

        // Native access (upscalers): handles, a texture's resource (VkImage) and
        // view (VkImageView), its transition to `state`, and InvalidateState after recording
        // native work (the backend sets its descriptor heaps, pipelines and targets again).
        virtual NativeHandles Native() = 0;
        // Whether the device was created with that extension (DeviceInfo::deviceExtensions the
        // driver lacks are left out).
        virtual bool DeviceExtensionEnabled(const char* name) const = 0;
        virtual void* NativeTexture(Texture* texture) = 0;
        virtual void* NativeView(Texture* texture) = 0;
        virtual void PrepareNative(Texture* texture, NativeState state) = 0;
        virtual void InvalidateState() = 0;

        virtual Timing TakeTiming() = 0;
        virtual void LogStats() = 0;
    };

    // Creates the device and the swap chain on `info.window`; null (and logged) if unavailable.
    Device* CreateVulkan(const DeviceInfo& info);

    // Compiles without a device (--test-shaders): true if `hlsl` compiles to SPIR-V.
    bool TestCompile(Stage stage, const std::string& hlsl, const char* name, Layout layout, const std::filesystem::path& shaderCache);

    // Shared by the backends (rhi_common.cpp).
    uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0);
    // Disk cache of compiled shaders: <cache>\<16 hex hash>.<extension>.
    bool ReadCachedShader(const std::filesystem::path& cache, uint64_t hash, const char* extension, std::vector<uint8_t>& data);
    void WriteCachedShader(const std::filesystem::path& cache, uint64_t hash, const char* extension, const void* data, size_t size);
    // Writes the HLSL of a shader that failed to userdata\shaderdump\<name>.failed.hlsl.
    void DumpFailedShader(const std::filesystem::path& cache, const char* name, const std::string& hlsl);
}
