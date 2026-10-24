// Shaders compiled ahead of use: see shader_warmup.h.

#include "shader_warmup.h"
#include "graphics_options.h"
#include "rhi.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace gpu::warmup
{
    namespace
    {
        struct Source
        {
            ShaderStage stage;
            uint64_t hash;
            std::vector<uint32_t> ucode;  // host order
        };

        std::filesystem::path UcodeDir(const std::filesystem::path& shaderCache)
        {
            return shaderCache / "ucode";
        }

        std::string Name(ShaderStage stage, uint64_t hash)
        {
            char name[64];
            snprintf(name, sizeof(name), "%s_%016llx", stage == ShaderStage::Pixel ? "ps" : "vs", (unsigned long long)hash);
            return name;
        }

        std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        uint32_t LoadBE(const std::vector<uint8_t>& data, size_t offset)
        {
            return offset + 4 <= data.size() ? (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) | (uint32_t(data[offset + 2]) << 8) | data[offset + 3]
                                             : 0u;
        }

        // The game's pixel shader files (the vertex shader files are not used as they are: D3D binds
        // them to vertex declarations first) and the recorded microcode.
        std::vector<Source> Sources(const std::filesystem::path& shaderCache)
        {
            std::vector<Source> sources;
            std::error_code error;
            std::filesystem::path folder = vfs::GameDir() / "shaders";
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
            {
                Source source;
                if (entry.path().extension() == ".out" && ReadShaderContainer(entry.path(), source.stage, source.ucode) &&
                    source.stage == ShaderStage::Pixel)
                {
                    source.hash = HashUcode(source.stage, source.ucode.data(), uint32_t(source.ucode.size()));
                    sources.push_back(std::move(source));
                }
            }
            for (const auto& entry : std::filesystem::directory_iterator(UcodeDir(shaderCache), error))
            {
                std::string stem = entry.path().stem().string();
                if (entry.path().extension() != ".ucode" || stem.size() != 19 || stem[2] != '_')
                    continue;
                std::vector<uint8_t> data = ReadFile(entry.path());
                if (data.empty() || data.size() % 4)
                    continue;
                Source source;
                source.stage = stem[0] == 'p' ? ShaderStage::Pixel : ShaderStage::Vertex;
                for (size_t i = 0; i < data.size(); i += 4)
                    source.ucode.push_back(LoadBE(data, i));
                source.hash = HashUcode(source.stage, source.ucode.data(), uint32_t(source.ucode.size()));
                if (Name(source.stage, source.hash) != stem)
                    continue;  // damaged or from another hash version
                sources.push_back(std::move(source));
            }
            std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.hash < b.hash; });
            sources.erase(std::unique(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.hash == b.hash; }),
                          sources.end());
            return sources;
        }

        // The renderer's translation (PrepareShader in xenos_renderer.cpp, without the user's
        // shaderoverride files) for one variant, compiled into the cache unless it is there.
        bool Compile(const Source& source, bool scaled, const std::filesystem::path& shaderCache)
        {
            TranslatedShader info = TranslateShader(source.stage, source.ucode.data(), uint32_t(source.ucode.size()), scaled);
            graphics::PatchShader(source.stage, source.hash, info.hlsl);
            std::string name = Name(source.stage, source.hash) + (scaled ? " (scaled)" : "");
            rhi::Stage stage = source.stage == ShaderStage::Pixel ? rhi::Stage::Pixel : rhi::Stage::Vertex;
            return rhi::TestCompile(stage, info.hlsl, name.c_str(), rhi::Layout::Draw, shaderCache);
        }

        // Every source in both variants, on `threads` threads. Returns the failures.
        int CompileAll(const std::vector<Source>& sources, const std::filesystem::path& shaderCache, uint32_t threads, int priority)
        {
            std::atomic<size_t> next{ 0 };
            std::atomic<int> failures{ 0 };
            auto work = [&] {
                if (priority != THREAD_PRIORITY_NORMAL)
                    SetThreadPriority(GetCurrentThread(), priority);
                for (size_t job; (job = next.fetch_add(1)) < sources.size() * 2;)
                    if (!Compile(sources[job / 2], job % 2 == 1, shaderCache))
                        failures++;
            };
            std::vector<std::thread> workers;
            for (uint32_t i = 1; i < threads; i++)
                workers.emplace_back(work);
            work();
            for (std::thread& worker : workers)
                worker.join();
            return failures;
        }
    }

    bool ReadShaderContainer(const std::filesystem::path& path, ShaderStage& stage, std::vector<uint32_t>& ucode)
    {
        std::vector<uint8_t> data = ReadFile(path);
        // Container: magic, size of the header block (the physical block follows it), ... The dword
        // at 0x18 points at the shader definition, whose first two dwords are the microcode offset in
        // the physical block and its size (literal constants come first).
        uint32_t magic = LoadBE(data, 0), definition = LoadBE(data, 0x18);
        uint32_t offset = LoadBE(data, 4) + LoadBE(data, definition), size = LoadBE(data, definition + 4);
        if ((magic & ~1u) != 0x102A1100 || uint64_t(offset) + size > data.size() || size % 4 || !size)
            return false;
        stage = (magic & 1) ? ShaderStage::Vertex : ShaderStage::Pixel;
        ucode.resize(size / 4);
        for (size_t i = 0; i < ucode.size(); i++)
            ucode[i] = LoadBE(data, offset + i * 4);
        return true;
    }

    void Record(ShaderStage stage, uint64_t hash, const uint32_t* hostOrderUcode, uint32_t dwordCount)
    {
        const std::filesystem::path& userData = vfs::UserDataDir();
        if (userData.empty() || !dwordCount)
            return;
        std::filesystem::path folder = UcodeDir(userData / "shadercache");
        std::filesystem::path path = folder / (Name(stage, hash) + ".ucode");
        std::error_code error;
        if (std::filesystem::exists(path, error))
            return;
        std::filesystem::create_directories(folder, error);
        std::vector<uint32_t> bigEndian(dwordCount);
        for (uint32_t i = 0; i < dwordCount; i++)
            bigEndian[i] = __builtin_bswap32(hostOrderUcode[i]);
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bigEndian.data()), std::streamsize(bigEndian.size() * 4));
            if (!file)
                return;
        }
        std::filesystem::rename(temporary, path, error);
    }

    void StartBackground(const std::filesystem::path& shaderCache)
    {
        std::thread([shaderCache] {
            SetThreadDescription(GetCurrentThread(), L"shader warm-up");
            auto start = std::chrono::steady_clock::now();
            std::vector<Source> sources = Sources(shaderCache);
            int failures = CompileAll(sources, shaderCache, 1, THREAD_PRIORITY_LOWEST);
            Log("[gpu] shader warm-up: %zu shaders x 2 variants checked in %.0f ms (%d failed)", sources.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), failures);
        }).detach();
    }

    int Precompile(const std::filesystem::path& shaderCache)
    {
        std::error_code error;
        std::filesystem::create_directories(shaderCache, error);
        auto start = std::chrono::steady_clock::now();
        std::vector<Source> sources = Sources(shaderCache);
        uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
        int failures = CompileAll(sources, shaderCache, threads, THREAD_PRIORITY_NORMAL);
        Log("[gpu] --precompile-shaders: %zu shaders x 2 variants in %.0f ms on %u threads, %d failed (cache %s)", sources.size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), threads, failures,
            shaderCache.string().c_str());
        return failures;
    }
}
