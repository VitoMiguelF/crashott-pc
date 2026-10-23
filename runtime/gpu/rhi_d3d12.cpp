// Direct3D 12 backend of the rendering hardware interface (rhi.h).
//
// One direct queue and command list; descriptors: CPU-only heaps for views (one SRV per texture view,
// RTVs, DSVs) copied into a shader-visible ring per draw, a shader-visible sampler heap holding blocks
// of 32 samplers cached by their descriptions, and the upload ring (one upload heap buffer) bound as a
// raw SRV (vertex data), as the index buffer and as the source of root CBVs and texture copies.
// Shaders are compiled with FXC (d3dcompiler_47.dll, loaded at run time, SM 5.1).

#include "rhi.h"
#include "rhi_ring.h"
#include "../diagnostics.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace gpu::rhi
{
    namespace
    {
        constexpr uint32_t BACK_BUFFER_COUNT = 2;
        constexpr uint64_t UPLOAD_RING_SIZE = 256ull << 20;
        constexpr uint32_t SHADER_VISIBLE_DESCRIPTORS = 65536;
        constexpr uint32_t VERTEX_DATA_DESCRIPTOR = SHADER_VISIBLE_DESCRIPTORS - 1;  // raw SRV of the whole upload ring
        constexpr uint32_t SAMPLER_DESCRIPTORS = 2048;
        constexpr uint32_t CPU_SRV_DESCRIPTORS = 32768;
        constexpr uint32_t RTV_DESCRIPTORS = 4096;
        constexpr uint32_t DSV_DESCRIPTORS = 1024;
        constexpr uint32_t TIMESTAMPS = 64;
        constexpr uint32_t NO_DESCRIPTOR = UINT32_MAX;

        constexpr D3D12_RESOURCE_STATES STATE_SHADER_RESOURCE =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

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

        // Indices of a CPU descriptor heap, with a free list (textures destroyed give theirs back).
        struct DescriptorPool
        {
            ComPtr<ID3D12DescriptorHeap> heap;
            uint32_t size = 0, count = 0, capacity = 0;
            std::vector<uint32_t> free;
            const char* name = "";

            uint32_t Allocate()
            {
                if (!free.empty())
                {
                    uint32_t index = free.back();
                    free.pop_back();
                    return index;
                }
                if (count >= capacity)
                    Fatal("[gpu] out of %s descriptors", name);
                return count++;
            }
            void Release(uint32_t index)
            {
                if (index != NO_DESCRIPTOR)
                    free.push_back(index);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += size_t(index) * size;
                return handle;
            }
        };

        class D3D12Texture : public Texture
        {
        public:
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
            uint32_t srv[3] = { NO_DESCRIPTOR, NO_DESCRIPTOR, NO_DESCRIPTOR };  // CPU SRV per View
            uint32_t rtv = NO_DESCRIPTOR, dsv = NO_DESCRIPTOR;
            bool backBuffer = false;
        };

        class D3D12Buffer : public Buffer
        {
        public:
            ComPtr<ID3D12Resource> resource;
        };

        class D3D12Shader : public Shader
        {
        public:
            ComPtr<ID3DBlob> blob;
            D3D12_SHADER_BYTECODE Bytecode() const { return { blob->GetBufferPointer(), blob->GetBufferSize() }; }
        };

        class D3D12Pipeline : public Pipeline
        {
        public:
            ComPtr<ID3D12PipelineState> state;
            ComPtr<ID3D12RootSignature> rootSignature;  // passes: their own
            // Passes.
            uint32_t textureCount = 0, constantBytes = 0;
            bool bufferInput = false;
        };

        // FXC, loaded at run time.
        pD3DCompile g_compile;
        HRESULT(WINAPI* g_createBlob)(SIZE_T, ID3DBlob**);
        bool g_compilerLoaded = false;

        void LoadCompiler()
        {
            if (g_compilerLoaded)
                return;
            g_compilerLoaded = true;
            HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
            g_compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
            g_createBlob = compiler ? reinterpret_cast<decltype(g_createBlob)>(GetProcAddress(compiler, "D3DCreateBlob")) : nullptr;
            if (!g_compile)
                Log("[gpu] d3dcompiler_47.dll not found: only cached shaders can be used");
        }

        const char* StageTarget(Stage stage)
        {
            return stage == Stage::Pixel ? "ps_5_1" : stage == Stage::Geometry ? "gs_5_1" : "vs_5_1";
        }

        ComPtr<ID3DBlob> CompileHlsl(const std::string& source, const char* target, const char* name, const std::filesystem::path& cache,
                                     uint64_t* compiles = nullptr, uint64_t* failures = nullptr)
        {
            LoadCompiler();
            uint64_t hash = HashBytes(source.data(), source.size(), HashBytes(target, strlen(target)));
            std::string extension = std::string(target) + ".dxbc";
            std::vector<uint8_t> cached;
            if (ReadCachedShader(cache, hash, extension.c_str(), cached))
            {
                ComPtr<ID3DBlob> blob;
                if (g_createBlob && SUCCEEDED(g_createBlob(cached.size(), &blob)))
                {
                    memcpy(blob->GetBufferPointer(), cached.data(), cached.size());
                    return blob;
                }
            }
            if (!g_compile)
                return nullptr;
            ComPtr<ID3DBlob> blob, errors;
            HRESULT result = g_compile(source.data(), source.size(), name, nullptr, nullptr, "main", target,
                                       D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors);
            if (compiles)
                (*compiles)++;
            if (FAILED(result))
            {
                if (failures)
                    (*failures)++;
                std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "?";
                if (message.size() > 1500)
                    message.resize(1500);
                Log("[gpu] shader %s failed to compile (%s):\n%s", name, target, message.c_str());
                DumpFailedShader(cache, name, source);
                return nullptr;
            }
            WriteCachedShader(cache, hash, extension.c_str(), blob->GetBufferPointer(), blob->GetBufferSize());
            return blob;
        }

        const char* FULLSCREEN_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

        D3D12_BLEND BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return D3D12_BLEND_ZERO;
            case 1: return D3D12_BLEND_ONE;
            case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;          // SrcColor
            case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;  // OneMinusSrcColor
            case 6: return D3D12_BLEND_SRC_ALPHA;
            case 7: return D3D12_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;        // DstColor
            case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
            case 10: return D3D12_BLEND_DEST_ALPHA;
            case 11: return D3D12_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;       // ConstantColor, ConstantAlpha
            case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;   // OneMinusConstantColor, OneMinusConstantAlpha
            case 16: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
            default: return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND_OP BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return D3D12_BLEND_OP_SUBTRACT;
            case 2: return D3D12_BLEND_OP_MIN;
            case 3: return D3D12_BLEND_OP_MAX;
            case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
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

        D3D12_TEXTURE_ADDRESS_MODE AddressMode(Address address)
        {
            switch (address)
            {
            case Address::Wrap: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case Address::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case Address::MirrorOnce: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
            case Address::Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
        }

        D3D12_SAMPLER_DESC SamplerOf(const SamplerDesc& sampler)
        {
            D3D12_SAMPLER_DESC desc{};
            if (sampler.anisotropy > 1)
            {
                desc.Filter = D3D12_FILTER_ANISOTROPIC;
                desc.MaxAnisotropy = sampler.anisotropy;
            }
            else
            {
                auto type = [](Filter filter) { return filter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT; };
                desc.Filter = D3D12_ENCODE_BASIC_FILTER(type(sampler.minFilter), type(sampler.magFilter), type(sampler.mipFilter),
                                                        D3D12_FILTER_REDUCTION_TYPE_STANDARD);
                desc.MaxAnisotropy = 1;
            }
            desc.AddressU = AddressMode(sampler.addressU);
            desc.AddressV = AddressMode(sampler.addressV);
            desc.AddressW = AddressMode(sampler.addressW);
            desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            float border = sampler.border == BorderColor::OpaqueWhite ? 1.0f : 0.0f;
            desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = border;
            desc.BorderColor[3] = sampler.border == BorderColor::TransparentBlack ? 0.0f : 1.0f;
            desc.MinLOD = sampler.minLod;
            desc.MaxLOD = sampler.maxLod;
            return desc;
        }

        D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(Topology topology)
        {
            switch (topology)
            {
            case Topology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case Topology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case Topology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case Topology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }
        }

        D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(Topology topology)
        {
            switch (topology)
            {
            case Topology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case Topology::LineList: case Topology::LineStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
        }

        bool IsDepthFormat(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        }

        class D3D12Device final : public Device
        {
        public:
            bool Init(const DeviceInfo& info);

            Backend Kind() const override { return Backend::D3D12; }
            const std::string& AdapterName() const override { return m_adapterName; }

            void Submit() override;
            void WaitIdle() override;
            Upload AllocateUpload(uint64_t size, uint64_t alignment) override;

            Texture* CreateTexture(const TextureDesc& desc) override;
            void DestroyTexture(Texture* texture) override;
            Texture* NullTexture() override { return nullptr; }
            Buffer* CreateBuffer(uint64_t size) override;
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

            DXGI_FORMAT BackBufferFormat() const override { return DXGI_FORMAT_R8G8B8A8_UNORM; }
            void BackBufferSize(uint32_t& width, uint32_t& height) const override
            {
                width = m_backBufferWidth;
                height = m_backBufferHeight;
            }
            bool Resize(uint32_t width, uint32_t height) override;
            Texture* BeginPresent() override;
            void Present(bool vsync) override;
            void UpdateFullscreen(bool wanted, bool focused) override;
            void LeaveFullscreen() override;
            bool ReadBack(Texture* texture, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;

            void WriteTimestamp(uint32_t index) override;
            void ResolveTimestamps(uint32_t first, uint32_t count) override;
            bool ReadTimestamps(uint32_t first, uint32_t count, uint64_t* ticks) override;
            double TimestampFrequency() const override { return double(m_timestampFrequency); }

            NativeHandles Native() override;
            void* NativeTexture(Texture* texture) override { return texture ? static_cast<D3D12Texture*>(texture)->resource.Get() : nullptr; }
            void* NativeView(Texture*) override { return nullptr; }
            void PrepareNative(Texture* texture, NativeState state) override;
            void InvalidateState() override
            {
                m_drawState.valid = false;
                m_heapsBound = false;
            }

            Timing TakeTiming() override
            {
                Timing timing = m_timing;
                m_timing = {};
                return timing;
            }
            void LogStats() override;

        private:
            void WaitForFence(uint64_t value);
            void RetireCompleted();
            uint32_t AllocateDescriptors(uint32_t count);
            D3D12_CPU_DESCRIPTOR_HANDLE GpuHeapCpu(uint32_t index) const;
            D3D12_GPU_DESCRIPTOR_HANDLE GpuHeapGpu(uint32_t index) const;
            void Transition(D3D12Texture* texture, D3D12_RESOURCE_STATES state);
            void BindDescriptorHeaps();
            uint32_t ViewSrv(D3D12Texture* texture, View view);
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv(D3D12Texture* texture);
            D3D12_CPU_DESCRIPTOR_HANDLE Dsv(D3D12Texture* texture);
            void ReleaseDescriptors(D3D12Texture* texture);
            void DrainDebugMessages();
            bool CreateDrawRootSignature();
            void CreateBackBuffers();
            void CollectGarbage();

            bool m_debugLayer = false;
            std::string m_adapterName;
            std::filesystem::path m_shaderCache;
            HWND m_hwnd = nullptr;
            ComPtr<IDXGIFactory4> m_factory;
            // DXGI_FEATURE_PRESENT_ALLOW_TEARING: with V-Sync off, Present(0) in a window that is not in
            // independent flip (unfocused, overlapped) is otherwise held to the monitor's refresh by DWM.
            bool m_allowTearing = false;
            ComPtr<ID3D12Device> m_device;
            ComPtr<ID3D12CommandQueue> m_queue;
            ComPtr<IDXGISwapChain3> m_swapChain;
            D3D12Texture m_backBuffers[BACK_BUFFER_COUNT];
            uint32_t m_backBufferWidth = 1280, m_backBufferHeight = 720;
            ComPtr<ID3D12Fence> m_fence;
            uint64_t m_fenceValue = 0;
            HANDLE m_fenceEvent = nullptr;
            ComPtr<ID3D12GraphicsCommandList> m_list;
            ComPtr<ID3D12CommandAllocator> m_allocator;
            std::deque<std::pair<ComPtr<ID3D12CommandAllocator>, uint64_t>> m_allocatorPool;
            std::deque<uint64_t> m_frameFences;
            bool m_heapsBound = false;

            // Descriptor heaps.
            DescriptorPool m_rtvs, m_dsvs, m_cpuSrvs;
            ComPtr<ID3D12DescriptorHeap> m_gpuSrvHeap, m_samplerHeap;
            uint32_t m_srvSize = 0, m_samplerSize = 0;
            uint32_t m_nullTextureSrv = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE m_nullRtv{};
            Ring m_descriptorRing;
            uint32_t m_samplerCount = 0;
            std::unordered_map<uint64_t, uint32_t> m_samplerBlocks;

            // Upload ring.
            ComPtr<ID3D12Resource> m_uploadBuffer;
            uint8_t* m_uploadCpu = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS m_uploadGpu = 0;
            Ring m_uploadRing;

            // Pipelines.
            ComPtr<ID3D12RootSignature> m_drawRootSignature;
            std::unordered_map<uint64_t, std::unique_ptr<D3D12Pipeline>> m_drawPipelines;
            std::vector<std::unique_ptr<D3D12Pipeline>> m_passPipelines;
            std::vector<std::unique_ptr<D3D12Shader>> m_shaders;
            ComPtr<ID3DBlob> m_fullscreenVs;

            // Timestamps.
            ComPtr<ID3D12QueryHeap> m_queryHeap;
            ComPtr<ID3D12Resource> m_queryReadback;
            uint64_t m_timestampFrequency = 0;

            // Released once the GPU is past the fence.
            struct Garbage
            {
                uint64_t fence;
                ComPtr<ID3D12Resource> resource;
                std::unique_ptr<D3D12Texture> texture;
                std::unique_ptr<D3D12Buffer> buffer;
            };
            std::deque<Garbage> m_garbage;

            Timing m_timing;
            struct
            {
                uint64_t submits = 0, pipelines = 0, shaderCompiles = 0, shaderFailures = 0;
            } m_stats;

            // Command list state set by the last Draw, so the next draw only records what changed
            // (~1,600 draws per frame in the village, ~17 calls each before). Everything else that
            // records graphics state (passes, clears, copies, native work, a new list) invalidates it.
            struct DrawListState
            {
                bool valid = false;
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
            } m_drawState;
        };

        // Submission ------------------------------------------------------------------------------

        void D3D12Device::DrainDebugMessages()
        {
            if (!m_debugLayer)
                return;
            ComPtr<ID3D12InfoQueue> queue;
            if (FAILED(m_device.As(&queue)))
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

        void D3D12Device::WaitForFence(uint64_t value)
        {
            if (m_fence->GetCompletedValue() >= value)
                return;
            m_fence->SetEventOnCompletion(value, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }

        void D3D12Device::RetireCompleted()
        {
            uint64_t completed = m_fence->GetCompletedValue();
            m_uploadRing.Retire(completed);
            m_descriptorRing.Retire(completed);
        }

        void D3D12Device::CollectGarbage()
        {
            uint64_t completed = m_fence->GetCompletedValue();
            while (!m_garbage.empty() && m_garbage.front().fence <= completed)
            {
                if (m_garbage.front().texture)
                    ReleaseDescriptors(m_garbage.front().texture.get());
                m_garbage.pop_front();
            }
        }

        void D3D12Device::Submit()
        {
            m_list->Close();
            ID3D12CommandList* lists[] = { m_list.Get() };
            m_queue->ExecuteCommandLists(1, lists);
            m_fenceValue++;
            m_queue->Signal(m_fence.Get(), m_fenceValue);
            m_uploadRing.OnSubmit(m_fenceValue);
            m_descriptorRing.OnSubmit(m_fenceValue);
            m_allocatorPool.push_back({ m_allocator, m_fenceValue });
            m_stats.submits++;

            m_allocator.Reset();
            uint64_t completed = m_fence->GetCompletedValue();
            if (!m_allocatorPool.empty() && m_allocatorPool.front().second <= completed)
            {
                m_allocator = m_allocatorPool.front().first;
                m_allocatorPool.pop_front();
                m_allocator->Reset();
            }
            else
            {
                m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator));
            }
            m_list->Reset(m_allocator.Get(), nullptr);
            InvalidateState();
            DrainDebugMessages();
            CollectGarbage();
        }

        void D3D12Device::WaitIdle()
        {
            Submit();
            WaitForFence(m_fenceValue);
            RetireCompleted();
            CollectGarbage();
        }

        Device::Upload D3D12Device::AllocateUpload(uint64_t size, uint64_t alignment)
        {
            // Fence values are only queried when the ring is full (GetCompletedValue was called
            // ~10,000 times per frame, once per allocation).
            uint64_t offset = m_uploadRing.TryAllocate(size, alignment);
            while (offset == UINT64_MAX)
            {
                RetireCompleted();
                offset = m_uploadRing.TryAllocate(size, alignment);
                if (offset != UINT64_MAX)
                    break;
                if (size > UPLOAD_RING_SIZE)
                    return {};
                LARGE_INTEGER waitStart, waitEnd;
                QueryPerformanceCounter(&waitStart);
                if (m_uploadRing.HasPending())
                    WaitForFence(m_uploadRing.OldestPendingFence());
                else
                    WaitIdle();
                QueryPerformanceCounter(&waitEnd);
                m_timing.ringWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
            }
            return { m_uploadCpu + offset, offset };
        }

        // Shader-visible CBV/SRV/UAV descriptors for this draw.
        uint32_t D3D12Device::AllocateDescriptors(uint32_t count)
        {
            uint64_t offset = m_descriptorRing.TryAllocate(count, 1);
            if (offset != UINT64_MAX)
                return uint32_t(offset);
            while (true)
            {
                RetireCompleted();
                offset = m_descriptorRing.TryAllocate(count, 1);
                if (offset != UINT64_MAX)
                    return uint32_t(offset);
                LARGE_INTEGER waitStart, waitEnd;
                QueryPerformanceCounter(&waitStart);
                if (m_descriptorRing.HasPending())
                    WaitForFence(m_descriptorRing.OldestPendingFence());
                else
                    WaitIdle();
                QueryPerformanceCounter(&waitEnd);
                m_timing.ringWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::GpuHeapCpu(uint32_t index) const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = m_gpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(index) * m_srvSize;
            return handle;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GpuHeapGpu(uint32_t index) const
        {
            D3D12_GPU_DESCRIPTOR_HANDLE handle = m_gpuSrvHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += uint64_t(index) * m_srvSize;
            return handle;
        }

        void D3D12Device::Transition(D3D12Texture* texture, D3D12_RESOURCE_STATES state)
        {
            if (texture->state == state)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture->resource.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = texture->state;
            barrier.Transition.StateAfter = state;
            m_list->ResourceBarrier(1, &barrier);
            texture->state = state;
        }

        void D3D12Device::BindDescriptorHeaps()
        {
            if (m_heapsBound)
                return;
            ID3D12DescriptorHeap* heaps[] = { m_gpuSrvHeap.Get(), m_samplerHeap.Get() };
            m_list->SetDescriptorHeaps(2, heaps);
            m_heapsBound = true;
        }

        // Resources -------------------------------------------------------------------------------

        // The typeless format of a color resource whose views differ (gamma render targets).
        DXGI_FORMAT ResourceFormat(const TextureDesc& desc)
        {
            if (desc.format == DXGI_FORMAT_D24_UNORM_S8_UINT)
                return DXGI_FORMAT_R24G8_TYPELESS;
            if (desc.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                return DXGI_FORMAT_R32G8X24_TYPELESS;
            if ((desc.usage & USAGE_RENDER_TARGET) &&
                (desc.format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
                return DXGI_FORMAT_R8G8B8A8_TYPELESS;
            return desc.format;
        }

        Texture* D3D12Device::CreateTexture(const TextureDesc& desc)
        {
            auto texture = std::make_unique<D3D12Texture>();
            texture->desc = desc;
            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = desc.width;
            resourceDesc.Height = desc.height;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = UINT16(desc.levels);
            resourceDesc.Format = ResourceFormat(desc);
            resourceDesc.SampleDesc.Count = 1;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_CLEAR_VALUE clear{};
            bool hasClear = false;
            if (desc.usage & USAGE_DEPTH_STENCIL)
            {
                resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                clear.Format = desc.format;
                clear.DepthStencil.Depth = 1.0f;
                hasClear = true;
                texture->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            else if (desc.usage & USAGE_RENDER_TARGET)
            {
                resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                clear.Format = desc.RenderFormat();
                hasClear = true;
                texture->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            else if (desc.usage & USAGE_STORAGE)
            {
                texture->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            else
            {
                texture->state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (desc.usage & USAGE_STORAGE)
                resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resourceDesc, texture->state, hasClear ? &clear : nullptr,
                                                         IID_PPV_ARGS(&texture->resource))))
            {
                Log("[gpu] could not create a %ux%u texture (%s, DXGI %u)", desc.width, desc.height, desc.name, uint32_t(desc.format));
                return nullptr;
            }
            if (desc.usage & USAGE_DEPTH_STENCIL)
            {
                m_list->ClearDepthStencilView(Dsv(texture.get()), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            }
            else if (desc.usage & USAGE_RENDER_TARGET)
            {
                const float zero[4] = {};
                m_list->ClearRenderTargetView(Rtv(texture.get()), zero, 0, nullptr);
            }
            return texture.release();
        }

        void D3D12Device::ReleaseDescriptors(D3D12Texture* texture)
        {
            for (uint32_t& srv : texture->srv)
            {
                m_cpuSrvs.Release(srv);
                srv = NO_DESCRIPTOR;
            }
            m_rtvs.Release(texture->rtv);
            m_dsvs.Release(texture->dsv);
            texture->rtv = texture->dsv = NO_DESCRIPTOR;
        }

        void D3D12Device::DestroyTexture(Texture* texture)
        {
            if (!texture)
                return;
            Garbage garbage{ m_fenceValue + 1 };
            garbage.texture.reset(static_cast<D3D12Texture*>(texture));
            m_garbage.push_back(std::move(garbage));
        }

        Buffer* D3D12Device::CreateBuffer(uint64_t size)
        {
            auto buffer = std::make_unique<D3D12Buffer>();
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RANGE noRead{ 0, 0 };
            if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&buffer->resource))) ||
                FAILED(buffer->resource->Map(0, &noRead, reinterpret_cast<void**>(&buffer->cpu))))
            {
                Log("[gpu] could not create a %llu-byte upload buffer (device removed reason 0x%08X)", (unsigned long long)size,
                    unsigned(m_device->GetDeviceRemovedReason()));
                return nullptr;
            }
            buffer->size = size;
            return buffer.release();
        }

        void D3D12Device::DestroyBuffer(Buffer* buffer)
        {
            if (!buffer)
                return;
            Garbage garbage{ m_fenceValue + 1 };
            garbage.buffer.reset(static_cast<D3D12Buffer*>(buffer));
            m_garbage.push_back(std::move(garbage));
        }

        uint32_t D3D12Device::ViewSrv(D3D12Texture* texture, View view)
        {
            if (!texture)
                return m_nullTextureSrv;
            uint32_t& index = texture->srv[uint32_t(view)];
            if (index != NO_DESCRIPTOR)
                return index;
            index = m_cpuSrvs.Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = texture->desc.levels;
            bool isFloat = texture->desc.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            switch (view)
            {
            case View::Depth:
                srv.Format = isFloat ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                break;
            case View::Stencil:
                srv.Format = isFloat ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                srv.Texture2D.PlaneSlice = 1;
                break;
            default:
                srv.Format = IsDepthFormat(texture->desc.format) ? (isFloat ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
                                                                  : texture->desc.format;
                if (srv.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && ResourceFormat(texture->desc) == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                break;
            }
            m_device->CreateShaderResourceView(texture->resource.Get(), &srv, m_cpuSrvs.Cpu(index));
            return index;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::Rtv(D3D12Texture* texture)
        {
            if (texture->rtv == NO_DESCRIPTOR)
            {
                texture->rtv = m_rtvs.Allocate();
                D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                rtv.Format = texture->desc.RenderFormat();
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                m_device->CreateRenderTargetView(texture->resource.Get(), &rtv, m_rtvs.Cpu(texture->rtv));
            }
            return m_rtvs.Cpu(texture->rtv);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::Dsv(D3D12Texture* texture)
        {
            if (texture->dsv == NO_DESCRIPTOR)
            {
                texture->dsv = m_dsvs.Allocate();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                dsv.Format = texture->desc.format;
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                m_device->CreateDepthStencilView(texture->resource.Get(), &dsv, m_dsvs.Cpu(texture->dsv));
            }
            return m_dsvs.Cpu(texture->dsv);
        }

        void D3D12Device::CopyUploadToTexture(Texture* texture, uint32_t level, uint64_t offset, uint32_t rowPitch)
        {
            auto* d3dTexture = static_cast<D3D12Texture*>(texture);
            Transition(d3dTexture, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_DESC desc = d3dTexture->resource->GetDesc();
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = m_uploadBuffer.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            m_device->GetCopyableFootprints(&desc, level, 1, 0, &source.PlacedFootprint, nullptr, nullptr, nullptr);
            source.PlacedFootprint.Offset = offset;
            source.PlacedFootprint.Footprint.RowPitch = rowPitch;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = d3dTexture->resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = level;
            m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }

        void D3D12Device::CopyTexture(Texture* destination, Texture* source, const Rect* region)
        {
            auto* to = static_cast<D3D12Texture*>(destination);
            auto* from = static_cast<D3D12Texture*>(source);
            Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(to, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION sourceLocation{}, destinationLocation{};
            sourceLocation.pResource = from->resource.Get();
            sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destinationLocation.pResource = to->resource.Get();
            destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            if (!region && from->desc.width == to->desc.width && from->desc.height == to->desc.height)
            {
                m_list->CopyResource(to->resource.Get(), from->resource.Get());
                return;
            }
            D3D12_BOX box{ 0, 0, 0, std::min(from->desc.width, to->desc.width), std::min(from->desc.height, to->desc.height), 1 };
            if (region)
                box = D3D12_BOX{ UINT(region->left), UINT(region->top), 0, UINT(region->right), UINT(region->bottom), 1 };
            m_list->CopyTextureRegion(&destinationLocation, box.left, box.top, 0, &sourceLocation, &box);
        }

        void D3D12Device::ClearColor(Texture* texture, const float color[4], const Rect* rect)
        {
            auto* d3dTexture = static_cast<D3D12Texture*>(texture);
            Transition(d3dTexture, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_RECT d3dRect = rect ? D3D12_RECT{ rect->left, rect->top, rect->right, rect->bottom } : D3D12_RECT{};
            m_list->ClearRenderTargetView(Rtv(d3dTexture), color, rect ? 1 : 0, rect ? &d3dRect : nullptr);
        }

        void D3D12Device::ClearDepthStencil(Texture* texture, float depth, uint8_t stencil, const Rect* rect)
        {
            auto* d3dTexture = static_cast<D3D12Texture*>(texture);
            Transition(d3dTexture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            D3D12_RECT d3dRect = rect ? D3D12_RECT{ rect->left, rect->top, rect->right, rect->bottom } : D3D12_RECT{};
            m_list->ClearDepthStencilView(Dsv(d3dTexture), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, stencil, rect ? 1 : 0,
                                          rect ? &d3dRect : nullptr);
        }

        // Shaders and pipelines -------------------------------------------------------------------

        Shader* D3D12Device::CreateShader(Stage stage, const std::string& hlsl, const char* name, Layout)
        {
            ComPtr<ID3DBlob> blob = CompileHlsl(hlsl, StageTarget(stage), name, m_shaderCache, &m_stats.shaderCompiles, &m_stats.shaderFailures);
            if (!blob)
                return nullptr;
            auto shader = std::make_unique<D3D12Shader>();
            shader->blob = blob;
            m_shaders.push_back(std::move(shader));
            return m_shaders.back().get();
        }

        bool D3D12Device::CreateDrawRootSignature()
        {
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
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
            {
                Log("[gpu] root signature: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
                return false;
            }
            return SUCCEEDED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_drawRootSignature)));
        }

        Pipeline* D3D12Device::GetDrawPipeline(const DrawPipelineKey& key)
        {
            uint64_t hash = HashBytes(&key, sizeof(key));
            auto it = m_drawPipelines.find(hash);
            if (it != m_drawPipelines.end())
                return it->second->state ? it->second.get() : nullptr;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = m_drawRootSignature.Get();
            desc.VS = static_cast<D3D12Shader*>(key.vs)->Bytecode();
            if (key.ps)
                desc.PS = static_cast<D3D12Shader*>(key.ps)->Bytecode();
            if (key.gs)
                desc.GS = static_cast<D3D12Shader*>(key.gs)->Bytecode();

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
            desc.RasterizerState.CullMode = key.cull == Cull::Front ? D3D12_CULL_MODE_FRONT : key.cull == Cull::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            desc.RasterizerState.FrontCounterClockwise = FALSE;
            desc.RasterizerState.DepthBias = key.depthBias;
            desc.RasterizerState.SlopeScaledDepthBias = key.slopeBias;
            desc.RasterizerState.DepthClipEnable = key.depthClip;

            uint32_t depthControl = key.depthControl;
            if (key.depthFormat)
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
                desc.DSVFormat = DXGI_FORMAT(key.depthFormat);
            }

            desc.IBStripCutValue = key.stripCut ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            desc.PrimitiveTopologyType = TopologyType(key.topology);
            desc.NumRenderTargets = key.colorCount;
            for (uint32_t i = 0; i < key.colorCount; i++)
                desc.RTVFormats[i] = DXGI_FORMAT(key.colorFormats[i]);
            desc.SampleDesc.Count = 1;

            auto pipeline = std::make_unique<D3D12Pipeline>();
            HRESULT result = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline->state));
            if (FAILED(result))
            {
                Log("[gpu] CreateGraphicsPipelineState failed (0x%08X) for vs %016llx ps %016llx", uint32_t(result),
                    (unsigned long long)key.vsHash, (unsigned long long)key.psHash);
                DrainDebugMessages();
            }
            m_stats.pipelines++;
            Pipeline* out = pipeline->state ? pipeline.get() : nullptr;
            m_drawPipelines[hash] = std::move(pipeline);
            return out;
        }

        Pipeline* D3D12Device::CreatePassPipeline(const PassPipelineDesc& desc)
        {
            auto pipeline = std::make_unique<D3D12Pipeline>();
            pipeline->textureCount = desc.textureCount;
            pipeline->constantBytes = desc.constantBytes;
            pipeline->bufferInput = desc.bufferInput;

            // Root signature: [0] CBV b0, [1] SRV table t0.. (or a root SRV for the buffer), static samplers.
            D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, std::max(desc.textureCount, 1u), 0, 0, 0 };
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor = { 0, 0 };
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            if (desc.bufferInput)
            {
                parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                parameters[1].Descriptor = { 0, 0 };
            }
            else
            {
                parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[1].DescriptorTable = { 1, &range };
            }
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_STATIC_SAMPLER_DESC samplers[2]{};
            for (uint32_t i = 0; i < desc.samplerCount; i++)
            {
                D3D12_SAMPLER_DESC sampler = SamplerOf(desc.samplers[i]);
                samplers[i].Filter = sampler.Filter;
                samplers[i].AddressU = sampler.AddressU;
                samplers[i].AddressV = sampler.AddressV;
                samplers[i].AddressW = sampler.AddressW;
                samplers[i].MaxAnisotropy = sampler.MaxAnisotropy;
                samplers[i].MinLOD = sampler.MinLOD;
                samplers[i].MaxLOD = sampler.MaxLOD;
                samplers[i].ShaderRegister = i;
                samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            }
            D3D12_ROOT_SIGNATURE_DESC rootDesc{ 2, parameters, desc.samplerCount, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
                FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->rootSignature))))
            {
                Log("[gpu] %s: could not create the root signature", desc.name.c_str());
                return nullptr;
            }

            ComPtr<ID3DBlob> vs = desc.vs.empty() ? m_fullscreenVs : CompileHlsl(desc.vs, "vs_5_1", (desc.name + "_vs").c_str(), m_shaderCache);
            ComPtr<ID3DBlob> ps = CompileHlsl(desc.ps, "ps_5_1", (desc.name + "_ps").c_str(), m_shaderCache);
            if (!vs || !ps)
                return nullptr;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = pipeline->rootSignature.Get();
            pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            for (uint32_t i = 0; i < desc.targetCount; i++)
            {
                D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[i];
                blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                if (desc.blend == Blend::Alpha)
                {
                    blend.BlendEnable = TRUE;
                    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                    blend.BlendOp = D3D12_BLEND_OP_ADD;
                    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
                    blend.DestBlendAlpha = D3D12_BLEND_ONE;
                    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                }
                pso.RTVFormats[i] = desc.targets[i];
            }
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = desc.targetCount;
            pso.SampleDesc.Count = 1;
            if (FAILED(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline->state))))
            {
                Log("[gpu] %s: could not create the pipeline", desc.name.c_str());
                DrainDebugMessages();
                return nullptr;
            }
            m_passPipelines.push_back(std::move(pipeline));
            return m_passPipelines.back().get();
        }

        // Drawing ---------------------------------------------------------------------------------

        void D3D12Device::Draw(const DrawCall& draw)
        {
            // Textures (t0..): their state first, then the descriptors.
            // Tables are declared with 32 entries; only the used ones are written.
            constexpr uint32_t srvCount = 32;
            uint32_t srvs[32];
            for (uint32_t i = 0; i < draw.textureCount; i++)
            {
                auto* texture = static_cast<D3D12Texture*>(draw.textures[i].texture);
                if (texture)
                    Transition(texture, STATE_SHADER_RESOURCE);
                srvs[i] = ViewSrv(texture, draw.textures[i].view);
            }
            uint32_t descriptors = AllocateDescriptors(srvCount);
            for (uint32_t i = 0; i < draw.textureCount; i++)
                m_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptors + i), m_cpuSrvs.Cpu(srvs[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            constexpr uint32_t samplerCount = 32;
            uint64_t samplerKey = HashBytes(draw.samplers, sizeof(SamplerDesc) * draw.textureCount, samplerCount);
            uint32_t samplerBase;
            auto samplerIt = m_samplerBlocks.find(samplerKey);
            if (samplerIt != m_samplerBlocks.end())
            {
                samplerBase = samplerIt->second;
            }
            else
            {
                if (m_samplerCount + samplerCount > SAMPLER_DESCRIPTORS)
                {
                    WaitIdle();
                    m_samplerBlocks.clear();
                    m_samplerCount = 0;
                }
                samplerBase = m_samplerCount;
                m_samplerCount += samplerCount;
                for (uint32_t i = 0; i < samplerCount; i++)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += size_t(samplerBase + i) * m_samplerSize;
                    D3D12_SAMPLER_DESC desc = SamplerOf(i < draw.textureCount ? draw.samplers[i] : SamplerDesc{});
                    m_device->CreateSampler(&desc, handle);
                }
                m_samplerBlocks[samplerKey] = samplerBase;
            }

            // Targets.
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
            for (uint32_t i = 0; i < draw.colorCount; i++)
            {
                if (auto* target = static_cast<D3D12Texture*>(draw.colorTargets[i]))
                {
                    Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    rtvs[i] = Rtv(target);
                }
                else
                {
                    rtvs[i] = m_nullRtv;
                }
            }
            auto* depthTarget = static_cast<D3D12Texture*>(draw.depthTarget);
            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
            if (depthTarget)
            {
                Transition(depthTarget, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                dsv = Dsv(depthTarget);
            }

            // Record.
            BindDescriptorHeaps();
            DrawListState& state = m_drawState;
            bool fresh = !state.valid;
            if (fresh)
            {
                // A (re)set root signature clears the root arguments: everything below is recorded.
                state = DrawListState{};
                m_list->SetGraphicsRootSignature(m_drawRootSignature.Get());
                D3D12_INDEX_BUFFER_VIEW ringView{ m_uploadGpu, uint32_t(UPLOAD_RING_SIZE), DXGI_FORMAT_R32_UINT };
                m_list->IASetIndexBuffer(&ringView);  // the whole upload ring; draws start at their indices
            }
            m_list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, m_uploadGpu + draw.systemConstants);
            m_list->SetGraphicsRootConstantBufferView(ROOT_VS_CONSTANTS, m_uploadGpu + draw.vertexConstants);
            m_list->SetGraphicsRootConstantBufferView(ROOT_PS_CONSTANTS, m_uploadGpu + draw.pixelConstants);
            m_list->SetGraphicsRootDescriptorTable(ROOT_TEXTURES, GpuHeapGpu(descriptors));
            if (fresh)
                m_list->SetGraphicsRootDescriptorTable(ROOT_VERTEX_DATA, GpuHeapGpu(VERTEX_DATA_DESCRIPTOR));
            D3D12_GPU_DESCRIPTOR_HANDLE samplerHandle = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
            samplerHandle.ptr += uint64_t(samplerBase) * m_samplerSize;
            if (fresh || state.samplerTable != samplerHandle.ptr)
            {
                m_list->SetGraphicsRootDescriptorTable(ROOT_SAMPLERS, samplerHandle);
                state.samplerTable = samplerHandle.ptr;
            }
            if (fresh || memcmp(&state.effects, &draw.effects, sizeof(draw.effects)) != 0)
            {
                m_list->SetGraphicsRoot32BitConstants(ROOT_EFFECTS, graphics::EFFECT_CONSTANT_DWORDS, &draw.effects, 0);
                state.effects = draw.effects;
            }
            ID3D12PipelineState* pipeline = static_cast<D3D12Pipeline*>(draw.pipeline)->state.Get();
            if (fresh || state.pipeline != pipeline)
            {
                m_list->SetPipelineState(pipeline);
                state.pipeline = pipeline;
            }
            if (fresh || state.renderTargetCount != draw.colorCount || state.dsv.ptr != dsv.ptr ||
                memcmp(state.rtvs, rtvs, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * draw.colorCount) != 0)
            {
                m_list->OMSetRenderTargets(draw.colorCount, draw.colorCount ? rtvs : nullptr, FALSE, depthTarget ? &dsv : nullptr);
                state.renderTargetCount = draw.colorCount;
                memcpy(state.rtvs, rtvs, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * draw.colorCount);
                state.dsv = dsv;
            }
            if (fresh || memcmp(state.blendFactor, draw.blendFactor, sizeof(draw.blendFactor)) != 0)
            {
                m_list->OMSetBlendFactor(draw.blendFactor);
                memcpy(state.blendFactor, draw.blendFactor, sizeof(draw.blendFactor));
            }
            if (fresh || state.stencilRef != draw.stencilRef)
            {
                m_list->OMSetStencilRef(draw.stencilRef);
                state.stencilRef = draw.stencilRef;
            }
            D3D12_VIEWPORT viewport{ draw.viewport.x, draw.viewport.y, draw.viewport.width, draw.viewport.height, draw.viewport.minDepth,
                                     draw.viewport.maxDepth };
            if (fresh || memcmp(&state.viewport, &viewport, sizeof(viewport)) != 0)
            {
                m_list->RSSetViewports(1, &viewport);
                state.viewport = viewport;
            }
            D3D12_RECT scissor{ draw.scissor.left, draw.scissor.top, draw.scissor.right, draw.scissor.bottom };
            if (fresh || memcmp(&state.scissor, &scissor, sizeof(scissor)) != 0)
            {
                m_list->RSSetScissorRects(1, &scissor);
                state.scissor = scissor;
            }
            D3D_PRIMITIVE_TOPOLOGY topology = PrimitiveTopology(draw.topology);
            if (fresh || state.topology != topology)
            {
                m_list->IASetPrimitiveTopology(topology);
                state.topology = topology;
            }
            state.valid = true;
            m_list->DrawIndexedInstanced(draw.indexCount, 1, draw.firstIndex, 0, 0);
        }

        void D3D12Device::DrawPass(const PassDraw& pass)
        {
            auto* pipeline = static_cast<D3D12Pipeline*>(pass.pipeline);
            m_drawState.valid = false;
            uint32_t descriptors = 0;
            if (!pipeline->bufferInput)
            {
                uint32_t srvs[8];
                for (uint32_t i = 0; i < pass.textureCount; i++)
                {
                    auto* texture = static_cast<D3D12Texture*>(pass.textures[i].texture);
                    if (texture)
                        Transition(texture, STATE_SHADER_RESOURCE);
                    srvs[i] = ViewSrv(texture, pass.textures[i].view);
                }
                uint32_t count = std::max(pipeline->textureCount, 1u);
                descriptors = AllocateDescriptors(count);
                for (uint32_t i = 0; i < count; i++)
                    m_device->CopyDescriptorsSimple(1, GpuHeapCpu(descriptors + i), m_cpuSrvs.Cpu(i < pass.textureCount ? srvs[i] : m_nullTextureSrv),
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            D3D12_GPU_VIRTUAL_ADDRESS constants = m_uploadGpu;
            if (pass.constantBytes)
            {
                Upload upload = AllocateUpload(AlignUp64(pass.constantBytes, 256), 256);
                if (!upload.cpu)
                    return;
                memcpy(upload.cpu, pass.constants, pass.constantBytes);
                constants = m_uploadGpu + upload.offset;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
            for (uint32_t i = 0; i < pass.targetCount; i++)
            {
                auto* target = static_cast<D3D12Texture*>(pass.targets[i]);
                Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
                rtvs[i] = Rtv(target);
            }
            BindDescriptorHeaps();
            m_list->SetGraphicsRootSignature(pipeline->rootSignature.Get());
            m_list->SetGraphicsRootConstantBufferView(0, constants);
            if (pipeline->bufferInput)
                m_list->SetGraphicsRootShaderResourceView(1, static_cast<D3D12Buffer*>(pass.buffer)->resource->GetGPUVirtualAddress() + pass.bufferOffset);
            else
                m_list->SetGraphicsRootDescriptorTable(1, GpuHeapGpu(descriptors));
            m_list->SetPipelineState(pipeline->state.Get());
            m_list->OMSetRenderTargets(pass.targetCount, rtvs, FALSE, nullptr);
            D3D12_VIEWPORT viewport{ pass.viewport.x, pass.viewport.y, pass.viewport.width, pass.viewport.height, 0.0f, 1.0f };
            D3D12_RECT scissor{ pass.scissor.left, pass.scissor.top, pass.scissor.right, pass.scissor.bottom };
            m_list->RSSetViewports(1, &viewport);
            m_list->RSSetScissorRects(1, &scissor);
            m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_list->DrawInstanced(pass.vertexCount, 1, 0, 0);
        }

        // Presentation ----------------------------------------------------------------------------

        void D3D12Device::CreateBackBuffers()
        {
            for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++)
            {
                D3D12Texture& buffer = m_backBuffers[i];
                m_swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer.resource));
                buffer.desc.width = m_backBufferWidth;
                buffer.desc.height = m_backBufferHeight;
                buffer.desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
                buffer.desc.usage = USAGE_RENDER_TARGET;
                buffer.desc.name = "back buffer";
                buffer.state = D3D12_RESOURCE_STATE_PRESENT;
                buffer.backBuffer = true;
                if (buffer.rtv == NO_DESCRIPTOR)
                    buffer.rtv = m_rtvs.Allocate();
                m_device->CreateRenderTargetView(buffer.resource.Get(), nullptr, m_rtvs.Cpu(buffer.rtv));
            }
        }

        bool D3D12Device::Resize(uint32_t width, uint32_t height)
        {
            if (!width || !height || (width == m_backBufferWidth && height == m_backBufferHeight))
                return false;
            WaitIdle();
            for (auto& buffer : m_backBuffers)
                buffer.resource.Reset();
            if (SUCCEEDED(m_swapChain->ResizeBuffers(BACK_BUFFER_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                     m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
            {
                m_backBufferWidth = width;
                m_backBufferHeight = height;
            }
            CreateBackBuffers();
            return true;
        }

        Texture* D3D12Device::BeginPresent()
        {
            return &m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()];
        }

        void D3D12Device::Present(bool vsync)
        {
            D3D12Texture* backBuffer = &m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()];
            Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT);
            LARGE_INTEGER submitStart, submitEnd, presentEnd;
            QueryPerformanceCounter(&submitStart);
            Submit();
            QueryPerformanceCounter(&submitEnd);
            // ALLOW_TEARING is invalid in exclusive fullscreen (there DXGI already presents unthrottled).
            BOOL exclusive = FALSE;
            m_swapChain->GetFullscreenState(&exclusive, nullptr);
            m_swapChain->Present(vsync ? 1 : 0, !vsync && m_allowTearing && !exclusive ? DXGI_PRESENT_ALLOW_TEARING : 0);
            QueryPerformanceCounter(&presentEnd);
            m_timing.submitTicks += uint64_t(submitEnd.QuadPart - submitStart.QuadPart);
            m_timing.presentTicks = std::max(m_timing.presentTicks, uint64_t(presentEnd.QuadPart - submitEnd.QuadPart));

            m_frameFences.push_back(m_fenceValue);
            LARGE_INTEGER waitStart, waitEnd;
            QueryPerformanceCounter(&waitStart);
            while (m_frameFences.size() > 2)
            {
                WaitForFence(m_frameFences.front());
                m_frameFences.pop_front();
            }
            QueryPerformanceCounter(&waitEnd);
            m_timing.frameWaitTicks += uint64_t(waitEnd.QuadPart - waitStart.QuadPart);
        }

        // video.window = fullscreen: exclusive swap chain fullscreen while the window has the
        // focus (DXGI drops it on Alt+Tab; the borderless window stays, and it is retried).
        void D3D12Device::UpdateFullscreen(bool wanted, bool focused)
        {
            BOOL fullscreen = FALSE;
            m_swapChain->GetFullscreenState(&fullscreen, nullptr);
            static ULONGLONG s_lastAttempt;
            if (wanted && !fullscreen && focused && GetTickCount64() - s_lastAttempt > 2000)
            {
                s_lastAttempt = GetTickCount64();
                HRESULT result = m_swapChain->SetFullscreenState(TRUE, nullptr);
                Log("[gpu] exclusive fullscreen: %s (0x%08X)", SUCCEEDED(result) ? "on" : "failed, staying borderless", uint32_t(result));
            }
            else if (!wanted && fullscreen)
            {
                m_swapChain->SetFullscreenState(FALSE, nullptr);
                Log("[gpu] exclusive fullscreen: off");
            }
        }

        void D3D12Device::LeaveFullscreen()
        {
            BOOL fullscreen = FALSE;
            if (SUCCEEDED(m_swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen)
                m_swapChain->SetFullscreenState(FALSE, nullptr);
        }

        bool D3D12Device::ReadBack(Texture* texture, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
        {
            auto* source = static_cast<D3D12Texture*>(texture);
            D3D12_RESOURCE_DESC desc = source->resource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
            uint64_t totalBytes;
            m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
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
            if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback))))
                return false;
            D3D12_RESOURCE_STATES before = source->state;
            Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
            sourceLocation.pResource = source->resource.Get();
            sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            m_list->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
            Transition(source, before);
            WaitIdle();
            void* data;
            D3D12_RANGE range{ 0, SIZE_T(totalBytes) };
            if (FAILED(readback->Map(0, &range, &data)))
                return false;
            width = footprint.Footprint.Width;
            height = footprint.Footprint.Height;
            rgba.resize(size_t(width) * height * 4);
            for (uint32_t y = 0; y < height; y++)
                memcpy(rgba.data() + size_t(y) * width * 4, static_cast<uint8_t*>(data) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                       size_t(width) * 4);
            readback->Unmap(0, nullptr);
            if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                for (size_t i = 0; i < rgba.size(); i += 4)
                    std::swap(rgba[i], rgba[i + 2]);
            }
            return true;
        }

        // Timestamps ------------------------------------------------------------------------------

        void D3D12Device::WriteTimestamp(uint32_t index)
        {
            if (m_queryHeap && index < TIMESTAMPS)
                m_list->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        }

        void D3D12Device::ResolveTimestamps(uint32_t first, uint32_t count)
        {
            if (m_queryHeap && first + count <= TIMESTAMPS)
                m_list->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, m_queryReadback.Get(), uint64_t(first) * 8);
        }

        bool D3D12Device::ReadTimestamps(uint32_t first, uint32_t count, uint64_t* ticks)
        {
            if (!m_queryHeap || first + count > TIMESTAMPS)
                return false;
            uint64_t* values = nullptr;
            D3D12_RANGE range{ size_t(first) * 8, size_t(first + count) * 8 };
            if (FAILED(m_queryReadback->Map(0, &range, reinterpret_cast<void**>(&values))))
                return false;
            memcpy(ticks, values + first, size_t(count) * 8);
            D3D12_RANGE none{ 0, 0 };
            m_queryReadback->Unmap(0, &none);
            return true;
        }

        // Native access ---------------------------------------------------------------------------

        NativeHandles D3D12Device::Native()
        {
            NativeHandles handles;
            handles.device = m_device.Get();
            handles.queue = m_queue.Get();
            handles.commandList = m_list.Get();
            return handles;
        }

        void D3D12Device::PrepareNative(Texture* texture, NativeState state)
        {
            static constexpr D3D12_RESOURCE_STATES states[] = { STATE_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET };
            if (texture)
                Transition(static_cast<D3D12Texture*>(texture), states[uint32_t(state)]);
        }

        void D3D12Device::LogStats()
        {
            Log("[gpu] d3d12: %llu submits, %llu pipelines, %llu shaders compiled (%llu failed), %u CPU SRVs, %u RTVs, %u DSVs",
                (unsigned long long)m_stats.submits, (unsigned long long)m_stats.pipelines, (unsigned long long)m_stats.shaderCompiles,
                (unsigned long long)m_stats.shaderFailures, m_cpuSrvs.count - uint32_t(m_cpuSrvs.free.size()), m_rtvs.count - uint32_t(m_rtvs.free.size()),
                m_dsvs.count - uint32_t(m_dsvs.free.size()));
        }

        // Startup ---------------------------------------------------------------------------------

        bool D3D12Device::Init(const DeviceInfo& info)
        {
            m_shaderCache = info.shaderCache;
            m_hwnd = HWND(info.window);
            m_debugLayer = info.debug;
            LoadCompiler();
            if (m_debugLayer)
            {
                ComPtr<ID3D12Debug> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                    debug->EnableDebugLayer();
            }
            if (FAILED(CreateDXGIFactory2(m_debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_factory))) ||
                FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
            {
                Log("[gpu] no Direct3D 12 device");
                return false;
            }
            if (m_debugLayer)
            {
                ComPtr<ID3D12InfoQueue> queue;
                if (SUCCEEDED(m_device.As(&queue)))
                    queue->SetMuteDebugOutput(FALSE);
            }
            DXGI_ADAPTER_DESC1 adapterDesc{};
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
                adapter->GetDesc1(&adapterDesc);
            char adapterName[128];
            WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
            m_adapterName = adapterName;

            D3D12_COMMAND_QUEUE_DESC queueDesc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
            m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
            m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
            m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator));
            m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_list));

            // Descriptor heaps.
            auto makeHeap = [this](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, ComPtr<ID3D12DescriptorHeap>& heap) {
                D3D12_DESCRIPTOR_HEAP_DESC desc{ type, count, shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
                return SUCCEEDED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
            };
            if (!makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_DESCRIPTORS, false, m_rtvs.heap) ||
                !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_DESCRIPTORS, false, m_dsvs.heap) ||
                !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CPU_SRV_DESCRIPTORS, false, m_cpuSrvs.heap) ||
                !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SHADER_VISIBLE_DESCRIPTORS, true, m_gpuSrvHeap) ||
                !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_DESCRIPTORS, true, m_samplerHeap))
            {
                Log("[gpu] could not create descriptor heaps");
                return false;
            }
            m_rtvs = { m_rtvs.heap, m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), 0, RTV_DESCRIPTORS, {}, "RTV" };
            m_dsvs = { m_dsvs.heap, m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV), 0, DSV_DESCRIPTORS, {}, "DSV" };
            m_cpuSrvs = { m_cpuSrvs.heap, m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV), 0, CPU_SRV_DESCRIPTORS, {},
                          "CPU SRV" };
            m_srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            m_samplerSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            m_descriptorRing.Init(VERTEX_DATA_DESCRIPTOR);  // the last slot is the upload ring's SRV

            // Null descriptors.
            m_nullTextureSrv = m_cpuSrvs.Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture{};
            nullTexture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullTexture.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(nullptr, &nullTexture, m_cpuSrvs.Cpu(m_nullTextureSrv));
            m_nullRtv = m_rtvs.Cpu(m_rtvs.Allocate());
            D3D12_RENDER_TARGET_VIEW_DESC nullRtv{};
            nullRtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_device->CreateRenderTargetView(nullptr, &nullRtv, m_nullRtv);

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
            if (FAILED(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&m_uploadBuffer))))
            {
                Log("[gpu] could not create the upload buffer");
                return false;
            }
            D3D12_RANGE noRead{ 0, 0 };
            m_uploadBuffer->Map(0, &noRead, reinterpret_cast<void**>(&m_uploadCpu));
            m_uploadGpu = m_uploadBuffer->GetGPUVirtualAddress();
            m_uploadRing.Init(UPLOAD_RING_SIZE);
            {
                // One raw SRV over the whole upload ring, in the last slot of the shader-visible heap
                // (the descriptor ring stops before it): a draw's vertex data is found by its offset in
                // the system constants' vertex buffer bases, instead of a CreateShaderResourceView per
                // draw (~7% of the command processor thread in the village).
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R32_TYPELESS;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.FirstElement = 0;
                srv.Buffer.NumElements = uint32_t(UPLOAD_RING_SIZE / 4);
                srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                m_device->CreateShaderResourceView(m_uploadBuffer.Get(), &srv, GpuHeapCpu(VERTEX_DATA_DESCRIPTOR));
            }

            if (!CreateDrawRootSignature())
            {
                Log("[gpu] could not create the draw root signature");
                return false;
            }
            m_fullscreenVs = CompileHlsl(FULLSCREEN_VS, "vs_5_1", "blit_vs", m_shaderCache);
            if (!m_fullscreenVs)
                return false;

            // Swap chain.
            DXGI_SWAP_CHAIN_DESC1 swapDesc{};
            swapDesc.Width = info.width;
            swapDesc.Height = info.height;
            swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapDesc.SampleDesc.Count = 1;
            swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDesc.BufferCount = BACK_BUFFER_COUNT;
            swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            {
                ComPtr<IDXGIFactory5> factory5;
                BOOL tearing = FALSE;
                if (SUCCEEDED(m_factory.As(&factory5)) &&
                    SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                    m_allowTearing = tearing != FALSE;
                if (m_allowTearing)
                    swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
                Log("[gpu] swap chain: tearing %s (frame rate above the monitor's refresh with V-Sync off, also when the window is not focused)",
                    m_allowTearing ? "allowed" : "not supported");
            }
            ComPtr<IDXGISwapChain1> swapChain;
            if (FAILED(m_factory->CreateSwapChainForHwnd(m_queue.Get(), m_hwnd, &swapDesc, nullptr, nullptr, &swapChain)) ||
                FAILED(swapChain.As(&m_swapChain)))
            {
                Log("[gpu] could not create the swap chain");
                return false;
            }
            m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
            m_backBufferWidth = info.width;
            m_backBufferHeight = info.height;
            CreateBackBuffers();

            // Timestamps.
            D3D12_QUERY_HEAP_DESC queryDesc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, TIMESTAMPS, 0 };
            bufferDesc.Width = TIMESTAMPS * sizeof(uint64_t);
            D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
            if (SUCCEEDED(m_queue->GetTimestampFrequency(&m_timestampFrequency)) && SUCCEEDED(m_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_queryHeap))))
            {
                if (FAILED(m_device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&m_queryReadback))))
                    m_queryHeap.Reset();
            }
            return true;
        }
    }

    Device* CreateD3D12(const DeviceInfo& info)
    {
        auto device = std::make_unique<D3D12Device>();
        if (!device->Init(info))
            return nullptr;
        return device.release();
    }

    bool TestCompileD3D12(Stage stage, const std::string& hlsl, const char* name, const std::filesystem::path& shaderCache)
    {
        return CompileHlsl(hlsl, StageTarget(stage), name, shaderCache) != nullptr;
    }
}
