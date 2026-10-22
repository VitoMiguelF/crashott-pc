// crash_assettool: the native half of the asset converters (tools/audio.py drives it).
//
//   crash_assettool rsd2wav <in.rsd> <out.wav>
//   crash_assettool rsd2wav --batch <list.txt> [--threads N]   lines: <in.rsd>\t<out.wav>
//   crash_assettool rsdinfo <in.rsd>
//   crash_assettool bik2mp4 <in.bik> <out.mp4> [--kbps N]       also writes <out>.trackN.wav
//
// Output lines are machine-readable: "ok <in> channels=.. rate=.. samples=.. source=.." or
// "error <in> <message>".

#include "assettool.h"
#include "ffmpeg_api.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

std::vector<uint8_t> ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool WriteFile(const fs::path& path, const void* data, size_t size)
{
    std::error_code error;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), error);
    fs::path temp = path;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        if (!out)
            return false;
        out.write(static_cast<const char*>(data), std::streamsize(size));
        if (!out)
            return false;
    }
    fs::rename(temp, path, error);
    return !error;
}

bool WriteWav(const fs::path& path, const int16_t* pcm, size_t frames, uint32_t channels, uint32_t sampleRate)
{
    uint32_t dataBytes = uint32_t(frames * channels * 2);
    std::vector<uint8_t> file(44 + dataBytes);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&file[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&file[at], &v, 2); };
    memcpy(&file[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(&file[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, uint16_t(channels));
    put32(24, sampleRate);
    put32(28, sampleRate * channels * 2);
    put16(32, uint16_t(channels * 2));
    put16(34, 16);
    memcpy(&file[36], "data", 4);
    put32(40, dataBytes);
    memcpy(&file[44], pcm, dataBytes);
    return WriteFile(path, file.data(), file.size());
}

namespace
{
    std::mutex g_print;

    std::string Escape(const std::string& text)
    {
        std::string out;
        for (char c : text)
            out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
        return out;
    }

    bool ConvertRsd(const fs::path& input, const fs::path& output)
    {
        try
        {
            RsdInfo info;
            std::vector<int16_t> pcm = DecodeRsd(ReadFile(input), info);
            if (!WriteWav(output, pcm.data(), info.decodedSamples, info.channels, info.sampleRate))
                throw std::runtime_error("cannot write " + output.string());
            std::lock_guard lock(g_print);
            printf("ok\t%s\tchannels=%u\trate=%u\tsamples=%u\tstreams=%u\tempty_frames=%u\tsource=%s\n",
                   input.string().c_str(), info.channels, info.sampleRate, info.decodedSamples, info.streams,
                   info.emptyFrames, Escape(info.sourcePath).c_str());
            return true;
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(g_print);
            printf("error\t%s\t%s\n", input.string().c_str(), e.what());
            return false;
        }
    }

    int Batch(const fs::path& list, unsigned threads)
    {
        std::vector<std::pair<fs::path, fs::path>> jobs;
        std::ifstream in(list);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            size_t tab = line.find('\t');
            if (tab == std::string::npos)
                continue;
            // Paths in the list are UTF-8.
            jobs.emplace_back(fs::path(std::u8string(line.begin(), line.begin() + tab).c_str()),
                              fs::path(std::u8string(line.begin() + tab + 1, line.end()).c_str()));
        }
        std::atomic<size_t> next = 0, failed = 0;
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < jobs.size();)
                    if (!ConvertRsd(jobs[i].first, jobs[i].second))
                        failed++;
            });
        for (auto& t : pool)
            t.join();
        fflush(stdout);
        return failed ? 1 : 0;
    }

    int Usage()
    {
        fprintf(stderr,
                "usage:\n"
                "  crash_assettool rsd2wav <in.rsd> <out.wav>\n"
                "  crash_assettool rsd2wav --batch <list.txt> [--threads N]\n"
                "  crash_assettool rsdinfo <in.rsd>\n"
                "  crash_assettool bik2mp4 <in.bik> <out.mp4> [--kbps N]\n");
        return 2;
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty())
        return Usage();
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    if (args[0] == L"rsd2wav" && args.size() >= 3 && args[1] == L"--batch")
    {
        unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        for (size_t i = 3; i + 1 < args.size(); i++)
            if (args[i] == L"--threads")
                threads = std::max(1, _wtoi(args[i + 1].c_str()));
        return Batch(args[2], threads);
    }
    if (args[0] == L"rsd2wav" && args.size() == 3)
        return ConvertRsd(args[1], args[2]) ? 0 : 1;
    if (args[0] == L"rsdinfo" && args.size() == 2)
    {
        RsdInfo i = ParseRsd(ReadFile(args[1]));
        printf("channels=%u rate=%u streams=%u", i.channels, i.sampleRate, i.streams);
        for (uint32_t c : i.streamChannels)
            printf(" [%u ch]", c);
        printf("\nplay begin=%u length=%u encoded=%u source=%u block size=%u blocks=%u data=0x%X+0x%X\npath=%s\n",
               i.playBegin, i.playLength, i.samplesEncoded, i.samplesInSource, i.blockSize, i.blockCount,
               i.dataOffset, i.dataSize, i.sourcePath.c_str());
        return 0;
    }
    if (args[0] == L"bik2mp4" && args.size() >= 3)
    {
        int kbps = 8000;
        for (size_t i = 3; i + 1 < args.size(); i++)
            if (args[i] == L"--kbps")
                kbps = _wtoi(args[i + 1].c_str());
        return ConvertBink(args[1], args[2], kbps);
    }
    return Usage();
}
