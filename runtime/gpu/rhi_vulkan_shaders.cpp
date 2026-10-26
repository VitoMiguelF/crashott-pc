// HLSL -> SPIR-V for the Vulkan backend (rhi_vulkan.cpp): DXC (dxcompiler.dll, loaded at run time)
// compiles the same HLSL as FXC does for Direct3D 12, with the registers mapped to descriptor sets
// and bindings per binding model (rhi::Layout):
//
//   Draw  set 0: t0-t31 -> bindings 0-31 (sampled images, push descriptors; the validation layers of
//                SDK 1.4.363 crash in vkCmdPushDescriptorSetKHR when the push set isn't set 0)
//         set 1: b0-b3 -> bindings 0-3 (dynamic uniform buffers), t0 space1 -> binding 4 (the upload
//                ring as a storage buffer, vertex data)
//         set 2: s0-s31 -> bindings 0-31 (samplers, cached sets)
//   Pass  set 0: b0 -> binding 0, t0-t7 -> bindings 1-8 (or the structured buffer at t0), s0-s1 ->
//                bindings 9-10 (immutable samplers); a regular set per pass
//
// D3D conventions kept: the backend flips the viewport (negative height) so clip space and
// SV_Position match Direct3D; -fvk-use-dx-position-w gives SV_Position.w as Direct3D does and
// -fvk-use-dx-layout the constant buffer packing.

#include "rhi_vulkan_shaders.h"
#include "../diagnostics.h"

#include "../platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>
#include <objidl.h>  // IStream (WIN32_LEAN_AND_MEAN leaves the COM headers out)
#endif
// Elsewhere dxcapi.h brings DXC's own WinAdapter.h (IUnknown, HRESULT, __uuidof).
// dxcapi.h's __declspec(uuid(...)) is unknown to mingw's clang (the GUIDs are declared below).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#include <dxcapi.h>
#pragma clang diagnostic pop

#ifdef __MINGW32__
// mingw's __uuidof needs the GUIDs declared (dxcapi.h only has __declspec(uuid), which MSVC-ABI compilers read).
__CRT_UUID_DECL(IDxcBlob, 0x8BA5FB08, 0x5195, 0x40E2, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02)
__CRT_UUID_DECL(IDxcBlobUtf8, 0x3DA636C9, 0xBA71, 0x4024, 0xA3, 0x01, 0x30, 0xCB, 0xF1, 0x25, 0x30, 0x5B)
__CRT_UUID_DECL(IDxcUtils, 0x4605C4CB, 0x2019, 0x492A, 0xAD, 0xA4, 0x65, 0xF2, 0x0B, 0xB7, 0xD6, 0x7F)
__CRT_UUID_DECL(IDxcResult, 0x58346CDA, 0xDDE7, 0x4497, 0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59)
__CRT_UUID_DECL(IDxcCompiler3, 0x228B4687, 0x5A6A, 0x4730, 0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54)
#endif

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gpu::rhi::vulkan
{
    namespace
    {
        // One compiler object per thread (DXC's objects are not thread-safe; the shader warm-up
        // compiles on worker threads, shader_warmup.cpp). g_compiler is the first one.
        std::once_flag g_loadOnce;
        DxcCreateInstanceProc g_create = nullptr;
        IDxcUtils* g_utils = nullptr;
        IDxcCompiler3* g_compiler = nullptr;
        thread_local IDxcCompiler3* t_compiler = nullptr;

        bool LoadCompilerOnce();

        bool LoadCompiler()
        {
            std::call_once(g_loadOnce, [] { LoadCompilerOnce(); });
            if (!g_compiler)
                return false;
            if (!t_compiler && FAILED(g_create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&t_compiler))))
                t_compiler = nullptr;
            return t_compiler != nullptr;
        }

        bool LoadCompilerOnce()
        {
#ifdef _WIN32
            void* module = plat::OpenLibrary("dxcompiler.dll");
#else
            void* module = plat::OpenLibrary("libdxcompiler.so");
#endif
            if (!module)
            {
                Log("[vulkan] dxcompiler not found next to the exe: only cached SPIR-V shaders can be used");
                return false;
            }
            auto create = reinterpret_cast<DxcCreateInstanceProc>(plat::FindSymbol(module, "DxcCreateInstance"));
            if (!create || FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(&g_utils))) ||
                FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&g_compiler))))
            {
                Log("[vulkan] dxcompiler.dll has no DxcCreateInstance / compiler");
                g_compiler = nullptr;
                return false;
            }
            g_create = create;
            t_compiler = g_compiler;
            return true;
        }

        const wchar_t* Profile(Stage stage)
        {
            return stage == Stage::Pixel ? L"ps_6_0" : stage == Stage::Geometry ? L"gs_6_0" : L"vs_6_0";
        }

        void BindRegister(std::vector<std::wstring>& args, const wchar_t* type, uint32_t number, uint32_t space, uint32_t binding, uint32_t set)
        {
            args.push_back(L"-fvk-bind-register");
            args.push_back(std::wstring(type) + std::to_wstring(number));
            args.push_back(std::to_wstring(space));
            args.push_back(std::to_wstring(binding));
            args.push_back(std::to_wstring(set));
        }

        std::vector<std::wstring> Arguments(Stage stage, Layout layout)
        {
            std::vector<std::wstring> args = { L"-spirv", L"-T", Profile(stage), L"-E", L"main", L"-HV", L"2018", L"-fspv-target-env=vulkan1.3",
                                               L"-fvk-use-dx-layout", L"-fvk-use-dx-position-w", L"-Wno-conversion", L"-Wno-ignored-attributes" };
            if (layout == Layout::Draw)
            {
                for (uint32_t b = 0; b < 4; b++)
                    BindRegister(args, L"b", b, 0, b, DRAW_SET_CONSTANTS);
                BindRegister(args, L"t", 0, 1, 4, DRAW_SET_CONSTANTS);
                for (uint32_t i = 0; i < 32; i++)
                {
                    BindRegister(args, L"t", i, 0, i, DRAW_SET_TEXTURES);
                    BindRegister(args, L"s", i, 0, i, DRAW_SET_SAMPLERS);
                }
            }
            else
            {
                BindRegister(args, L"b", 0, 0, 0, 0);
                for (uint32_t i = 0; i < 8; i++)
                    BindRegister(args, L"t", i, 0, 1 + i, 0);
                BindRegister(args, L"s", 0, 0, 9, 0);
                BindRegister(args, L"s", 1, 0, 10, 0);
            }
            return args;
        }

        // Bump when the arguments change: part of the cache key.
        constexpr const char* CACHE_VERSION = "spirv-2";
    }

    uint64_t SpirvHash(Stage stage, const std::string& hlsl, Layout layout)
    {
        char keyText[64];
        snprintf(keyText, sizeof(keyText), "%s/%d/%d", CACHE_VERSION, int(stage), int(layout));
        return HashBytes(hlsl.data(), hlsl.size(), HashBytes(keyText, strlen(keyText)));
    }

    bool CompileSpirv(Stage stage, const std::string& hlsl, const char* name, Layout layout, const std::filesystem::path& cache,
                      std::vector<uint32_t>& spirv, uint64_t* compiles, uint64_t* failures)
    {
        uint64_t hash = SpirvHash(stage, hlsl, layout);
        std::vector<uint8_t> cached;
        if (ReadCachedShader(cache, hash, "spv", cached) && cached.size() % 4 == 0)
        {
            spirv.resize(cached.size() / 4);
            memcpy(spirv.data(), cached.data(), cached.size());
            return true;
        }
        if (!LoadCompiler())
        {
            // Android (cached SPIR-V only): which shaders the PC's cache lacked.
            static std::atomic<uint32_t> misses;
            if (misses.fetch_add(1) < 64)
                Log("[vulkan] %s (spv %016llx) is not in the shader cache and there is no compiler", name ? name : "?",
                    (unsigned long long)hash);
            if (failures)
                (*failures)++;
            return false;
        }

        std::vector<std::wstring> args = Arguments(stage, layout);
        std::vector<const wchar_t*> argPointers;
        for (const std::wstring& arg : args)
            argPointers.push_back(arg.c_str());
        DxcBuffer source{ hlsl.data(), hlsl.size(), DXC_CP_UTF8 };
        IDxcResult* result = nullptr;
        if (compiles)
            (*compiles)++;
        HRESULT status = t_compiler->Compile(&source, argPointers.data(), uint32_t(argPointers.size()), nullptr, __uuidof(IDxcResult),
                                             reinterpret_cast<void**>(&result));
        if (SUCCEEDED(status) && result)
            result->GetStatus(&status);
        IDxcBlob* code = nullptr;
        if (SUCCEEDED(status) && result)
            result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&code), nullptr);
        if (FAILED(status) || !code || code->GetBufferSize() % 4)
        {
            if (failures)
                (*failures)++;
            std::string message = "?";
            IDxcBlobUtf8* errors = nullptr;
            if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&errors), nullptr)) && errors)
            {
                message.assign(errors->GetStringPointer(), errors->GetStringLength());
                errors->Release();
            }
            if (message.size() > 1500)
                message.resize(1500);
            Log("[vulkan] shader %s failed to compile:\n%s", name, message.c_str());
            DumpFailedShader(cache, name, hlsl);
            if (code)
                code->Release();
            if (result)
                result->Release();
            return false;
        }
        spirv.resize(code->GetBufferSize() / 4);
        memcpy(spirv.data(), code->GetBufferPointer(), code->GetBufferSize());
        WriteCachedShader(cache, hash, "spv", spirv.data(), spirv.size() * 4);
        code->Release();
        result->Release();
        return true;
    }

    bool CompilerAvailable()
    {
        if (!LoadCompiler())
            return false;
        static const char HLSL[] = "float4 main() : SV_Target { return 0; }";
        std::vector<std::wstring> args = Arguments(Stage::Pixel, Layout::Pass);
        std::vector<const wchar_t*> argPointers;
        for (const std::wstring& arg : args)
            argPointers.push_back(arg.c_str());
        DxcBuffer source{ HLSL, sizeof(HLSL) - 1, DXC_CP_UTF8 };
        IDxcResult* result = nullptr;
        HRESULT status = t_compiler->Compile(&source, argPointers.data(), uint32_t(argPointers.size()), nullptr, __uuidof(IDxcResult),
                                             reinterpret_cast<void**>(&result));
        if (SUCCEEDED(status) && result)
            result->GetStatus(&status);
        if (result)
            result->Release();
        if (FAILED(status))
            Log("[vulkan] dxcompiler.dll does not generate SPIR-V (the Windows SDK's copy? the Vulkan SDK's or a DirectXShaderCompiler release's is needed)");
        return SUCCEEDED(status);
    }
}

namespace gpu::rhi
{
    bool TestCompile(Stage stage, const std::string& hlsl, const char* name, Layout layout, const std::filesystem::path& shaderCache)
    {
        std::vector<uint32_t> spirv;
        return vulkan::CompileSpirv(stage, hlsl, name, layout, shaderCache, spirv);
    }
}
