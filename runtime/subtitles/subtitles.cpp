#include "subtitles.h"

#include "../audio/audio_output.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../paths.h"
#include "../settings/settings.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>


namespace fs = std::filesystem;

namespace subtitles
{
    namespace
    {
        constexpr uint32_t CONTEXTS = 320;           // XMA decoder contexts (audio/xma.cpp)
        constexpr uint32_t PACKET_BYTES = 2048;
        constexpr uint32_t BLOCK_PACKETS = 32;       // XMA2 blocks of 64 KB
        constexpr uint32_t HASHED_PACKETS = 8;       // fingerprint = the first 8 packets of a buffer
        constexpr uint32_t SAMPLES_PER_FRAME = 512;
        constexpr double PAUSE_SECONDS = 3.5;   // stream not decoding for this long: paused or skipped
                                                // (the title decodes cutscene audio in ~2.3 s bursts)
        constexpr double FORGET_SECONDS = 10.0; // ...for this long: the track ends
        // Subtitled sounds started on one context in a quick row (BULK_COUNT within BULK_SECONDS) are
        // being unpacked into memory, not played (load time); a clip decoded whole waits
        // HOLD_SECONDS before showing, so such a row never flashes on screen. (The dialogue clips seen
        // so far are decoded when played: the episode title, the cutscenes, Aku Aku's tutorial.)
        constexpr double BULK_SECONDS = 0.5;
        constexpr size_t BULK_COUNT = 3;
        constexpr double HOLD_SECONDS = 0.2;

        struct Line
        {
            double start, end;
            std::wstring text;
        };

        struct Sound
        {
            std::string key;
            std::string archive;  // "english"
            std::string entry;    // "sound\rsd\english\c\c268899e25ed5279.rsd"
            double seconds = 0.0;
            std::vector<Line> lines;
            // From the .rsd (fingerprinting thread).
            uint32_t rate = 0;
            std::vector<uint64_t> blockStart;  // first sample of each block (per stream)
        };

        struct Ref
        {
            int sound;
            int block;
            int stream;    // stream whose first packet the buffer starts at (block 0); -1 = block start
            bool ambiguous;
        };

        struct Track
        {
            int sound;
            uint32_t context;
            uint64_t startFrames;   // frames the context had decoded when the sound started
            double startSeconds;    // position of the first submitted block in the sound
            uint32_t rate;
            double startWall;       // Now() at the start
            uint64_t lastAudio;     // audio::SubmittedFrames() at the last update
            double clock;           // playback position: follows the title's audio clock, never past
                                    // what was decoded, frozen while the stream doesn't decode
            bool detached = false;  // the context moved on after decoding the whole sound
            double decodedEnd = 0.0;
        };

        std::vector<Sound> g_sounds;
        std::string g_source;
        std::unordered_map<uint64_t, Ref> g_refs;
        std::atomic<bool> g_ready{ false };  // g_refs complete
        // Delay from the sound's first decoded packet to hearing it: the title starts the voice ~0.24 s
        // after it hands over the first buffer (measured against the audio dump of the intro scene),
        // plus the output queue (~43 ms). CRASH_SUBTITLE_OFFSET_MS replaces it.
        double g_offset = 0.28;
        bool g_trace = false;                // CRASH_SUBTITLE_TRACE=1
        std::wstring g_testText;             // CRASH_SUBTITLE_TEST=<text>: always shown (style checks)

        std::atomic<uint64_t> g_frames[CONTEXTS];
        std::atomic<int64_t> g_lastAdvance[CONTEXTS];  // QPC of the last decoded frame
        LARGE_INTEGER g_frequency;

        std::mutex g_mutex;  // tracks, g_recentStarts
        std::unordered_map<uint32_t, std::vector<double>> g_recentStarts;  // per context: Now() of subtitled starts
        std::vector<Track> g_tracks;
        std::wstring g_text;
        bool g_gameplay = false;  // g_text has a line of an in-game clip (not a cutscene)
        uint64_t g_version = 0;

        uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        uint32_t LE32(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        std::string Lower(std::string s)
        {
            for (char& c : s)
                c = char(tolower(uint8_t(c)));
            return s;
        }

        std::string Trim(const std::string& s)
        {
            size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }

        std::wstring Widen(const std::string& s)
        {
            if (s.empty())
                return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
            // "\n" in the file = line break.
            for (size_t i = 0; (i = w.find(L"\\n", i)) != std::wstring::npos;)
                w.replace(i, 2, L"\n");
            return w;
        }

        uint64_t Fnv(const uint8_t* data, size_t size)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < size; i++)
                h = (h ^ data[i]) * 0x100000001B3ull;
            return h;
        }

        double Now()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return double(t.QuadPart) / double(g_frequency.QuadPart);
        }

        // Subtitle file: [key] blocks with sound/seconds and "start end text" lines. Entries of
        // `sounds` with the same key are replaced field by field (a translation over the English).
        bool ReadFile(const fs::path& path, std::vector<Sound>& sounds, bool overlay)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::string raw;
            Sound* current = nullptr;
            bool replacedLines = false;
            int lineNumber = 0, bad = 0;
            while (std::getline(in, raw))
            {
                lineNumber++;
                if (lineNumber == 1 && raw.size() >= 3 && uint8_t(raw[0]) == 0xEF && uint8_t(raw[1]) == 0xBB && uint8_t(raw[2]) == 0xBF)
                    raw.erase(0, 3);
                std::string line = Trim(raw);
                if (line.empty() || line[0] == '#')
                    continue;
                if (line.front() == '[' && line.back() == ']')
                {
                    std::string key = line.substr(1, line.size() - 2);
                    auto it = std::find_if(sounds.begin(), sounds.end(), [&](const Sound& s) { return s.key == key; });
                    if (it == sounds.end())
                    {
                        sounds.push_back({});
                        sounds.back().key = key;
                        current = &sounds.back();
                    }
                    else
                        current = &*it;
                    replacedLines = false;
                    continue;
                }
                if (!current)
                {
                    bad++;
                    continue;
                }
                if (line.rfind("sound", 0) == 0 && line.find('=') != std::string::npos)
                {
                    std::istringstream value(Trim(line.substr(line.find('=') + 1)));
                    value >> current->archive >> current->entry;
                    current->archive = Lower(current->archive);
                    current->entry = Lower(current->entry);
                    continue;
                }
                if (line.rfind("seconds", 0) == 0 && line.find('=') != std::string::npos)
                {
                    current->seconds = atof(line.substr(line.find('=') + 1).c_str());
                    continue;
                }
                double start, end;
                int used = 0;
                if (sscanf(line.c_str(), "%lf %lf %n", &start, &end, &used) < 2 || used <= 0 || end < start)
                {
                    if (bad++ < 5)
                        Log("[subtitles] %s:%d: expected \"start end text\"", path.string().c_str(), lineNumber);
                    continue;
                }
                if (overlay && !replacedLines)
                    current->lines.clear();  // a translated key replaces the English lines
                replacedLines = true;
                current->lines.push_back({ start, end, Widen(line.substr(size_t(used))) });
            }
            return true;
        }

        // Minimal reader of a Radical .rcf (tools/rcf.py): entry name -> (offset, size).
        struct RcfIndex
        {
            fs::path host;
            std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> entries;
        };

        bool ReadExact(HANDLE file, uint64_t offset, void* data, uint32_t size)
        {
            OVERLAPPED o{};
            o.Offset = DWORD(offset);
            o.OffsetHigh = DWORD(offset >> 32);
            DWORD read = 0;
            return ReadFile(file, data, size, &read, &o) && read == size;
        }

        bool OpenRcf(const std::string& archive, RcfIndex& index, HANDLE& file)
        {
            auto path = vfs::Parse("D:\\" + archive + ".rcf");
            auto location = path ? vfs::Locate(*path) : std::nullopt;
            if (!location)
                return false;
            index.host = location->host;
            file = CreateFileW(index.host.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            uint8_t header[0x3C];
            if (!ReadExact(file, 0, header, sizeof(header)) || memcmp(header, "ATG CORE CEMENT LIBRARY", 23) != 0)
                return false;
            uint32_t dirOffset = BE32(header + 0x24), namesOffset = BE32(header + 0x2C), namesSize = BE32(header + 0x30),
                     count = BE32(header + 0x38);
            std::vector<uint8_t> directory(size_t(count) * 12), names(namesSize);
            if (!ReadExact(file, dirOffset, directory.data(), uint32_t(directory.size())) || !ReadExact(file, namesOffset, names.data(), namesSize))
                return false;
            // Names are listed in data-offset order.
            std::vector<std::pair<uint32_t, uint32_t>> byOffset(count);
            for (uint32_t i = 0; i < count; i++)
                byOffset[i] = { BE32(&directory[i * 12 + 4]), BE32(&directory[i * 12 + 8]) };
            std::sort(byOffset.begin(), byOffset.end());
            size_t pos = 12;
            for (uint32_t i = 0; i < count && pos + 12 <= names.size(); i++)
            {
                uint32_t length = LE32(&names[pos + 8]);
                pos += 12;
                if (pos + length > names.size())
                    break;
                std::string name(reinterpret_cast<const char*>(&names[pos]), strnlen(reinterpret_cast<const char*>(&names[pos]), length));
                index.entries[Lower(name)] = byOffset[i];
                pos += length + 3 + 4;
            }
            return true;
        }

        // Reads every subtitled .rsd and records the fingerprints of block 0 (one per stream) and
        // of the start of every later block.
        void Fingerprint()
        {
            std::unordered_map<uint64_t, Ref> refs;
            std::unordered_map<std::string, std::pair<RcfIndex, HANDLE>> archives;
            int found = 0, missing = 0;
            for (size_t s = 0; s < g_sounds.size(); s++)
            {
                Sound& sound = g_sounds[s];
                if (sound.archive.empty() || sound.entry.empty())
                {
                    missing++;
                    continue;
                }
                auto it = archives.find(sound.archive);
                if (it == archives.end())
                {
                    RcfIndex index;
                    HANDLE file = INVALID_HANDLE_VALUE;
                    if (!OpenRcf(sound.archive, index, file))
                    {
                        Log("[subtitles] cannot read %s.rcf: sounds from it get no subtitles", sound.archive.c_str());
                        if (file != INVALID_HANDLE_VALUE)
                            CloseHandle(file);
                        file = INVALID_HANDLE_VALUE;
                    }
                    it = archives.emplace(sound.archive, std::make_pair(std::move(index), file)).first;
                }
                HANDLE file = it->second.second;
                auto entry = it->second.first.entries.find(sound.entry);
                if (file == INVALID_HANDLE_VALUE || entry == it->second.first.entries.end() || entry->second.second < 0x80C)
                {
                    missing++;
                    continue;
                }
                std::vector<uint8_t> data(entry->second.second);
                if (!ReadExact(file, entry->second.first, data.data(), uint32_t(data.size())) || memcmp(data.data(), "RSD6XMA ", 8) != 0)
                {
                    missing++;
                    continue;
                }
                // 0x800 header (channels, bits, rate at 0x08); at 0x800: XMA2 format size, seek table size, data size.
                uint32_t formatSize = BE32(&data[0x800]), seekSize = BE32(&data[0x804]), dataSize = BE32(&data[0x808]);
                size_t start = 0x80C + size_t(formatSize) + seekSize;
                if (formatSize < 2 || start > data.size())
                {
                    missing++;
                    continue;
                }
                sound.rate = LE32(&data[0x10]);
                uint32_t streams = std::max<uint32_t>(1, data[0x80C + 1]);
                size_t available = std::min<size_t>(dataSize, data.size() - start);
                uint32_t packets = uint32_t(available / PACKET_BYTES);
                uint32_t blocks = (packets + BLOCK_PACKETS - 1) / BLOCK_PACKETS;
                sound.blockStart.assign(blocks, 0);
                for (uint32_t b = 1; b < blocks && b * 4 <= seekSize; b++)
                    sound.blockStart[b] = BE32(&data[0x80C + formatSize + (b - 1) * 4]);

                auto add = [&](uint64_t hash, int block, int stream) {
                    auto [r, inserted] = refs.emplace(hash, Ref{ int(s), block, stream, false });
                    if (!inserted && (r->second.sound != int(s) || r->second.block != block))
                        r->second.ambiguous = true;
                };
                for (uint32_t b = 0; b < blocks; b++)
                {
                    uint32_t blockPackets = std::min(BLOCK_PACKETS, packets - b * BLOCK_PACKETS);
                    const uint8_t* block = &data[start + size_t(b) * BLOCK_PACKETS * PACKET_BYTES];
                    for (uint32_t stream = 0; stream < (b == 0 ? streams : 1); stream++)
                    {
                        if (stream >= blockPackets)
                            break;
                        uint32_t n = std::min(HASHED_PACKETS, blockPackets - stream);
                        add(Fnv(block + size_t(stream) * PACKET_BYTES, size_t(n) * PACKET_BYTES), int(b), b == 0 ? int(stream) : -1);
                    }
                }
                found++;
            }
            for (auto& [name, a] : archives)
                if (a.second != INVALID_HANDLE_VALUE)
                    CloseHandle(a.second);
            size_t ambiguous = std::count_if(refs.begin(), refs.end(), [](const auto& r) { return r.second.ambiguous; });
            g_refs = std::move(refs);
            g_ready = true;
            Log("[subtitles] %d sound(s) recognizable (%zu fingerprints, %zu ambiguous), %d not found in the archives", found,
                g_refs.size(), ambiguous, missing);
        }

        fs::path FindFile(const settings::Language& language, fs::path& english)
        {
            for (fs::path candidate : { paths::Data() / "subtitles" / "english.txt", vfs::UserDataDir() / "subtitles" / "english.txt" })
            {
                if (fs::exists(candidate))
                {
                    english = candidate.make_preferred();
                    break;
                }
            }
            if (const char* file = getenv("CRASH_SUBTITLE_FILE"); file && *file)
                return fs::path(file);
            if (language.pack)
                return fs::path(language.pack) / "subtitles.txt";
            if (strcmp(language.voiceRcf, "english.rcf") != 0)
                return {};  // other voice languages: the English transcript doesn't match
            return english;
        }
    }

    void Init()
    {
        QueryPerformanceFrequency(&g_frequency);
        if (const char* offset = getenv("CRASH_SUBTITLE_OFFSET_MS"))
            g_offset = atof(offset) / 1000.0;
        g_trace = getenv("CRASH_SUBTITLE_TRACE") && strcmp(getenv("CRASH_SUBTITLE_TRACE"), "0") != 0;
        wchar_t test[512];
        if (DWORD n = GetEnvironmentVariableW(L"CRASH_SUBTITLE_TEST", test, 512); n > 0 && n < 512)
        {
            g_testText.assign(test, n);
            for (size_t i = 0; (i = g_testText.find(L"\\n", i)) != std::wstring::npos;)
                g_testText.replace(i, 2, L"\n");
        }

        const settings::Language& language = settings::StartupLanguage();
        fs::path english;
        fs::path file = FindFile(language, english);
        if (file.empty() || !fs::exists(file))
        {
            Log("[subtitles] no subtitles for %s (%s): tools\\subtitles.py makes them (README \"Legendas\")", language.id,
                file.empty() ? "voices not in English" : file.string().c_str());
            return;
        }
        if (!english.empty() && english != file)
            ReadFile(english, g_sounds, false);
        ReadFile(file, g_sounds, !english.empty() && english != file);
        g_source = file.string();
        size_t lines = 0;
        for (const Sound& s : g_sounds)
            lines += s.lines.size();
        Log("[subtitles] %s: %zu sound(s), %zu line(s)%s", g_source.c_str(), g_sounds.size(), lines,
            !english.empty() && english != file ? " (missing fields from the English file)" : "");
        if (g_sounds.empty())
            return;
        std::thread(Fingerprint).detach();
    }

    size_t SoundCount()
    {
        return g_testText.empty() ? g_sounds.size() : std::max<size_t>(g_sounds.size(), 1);
    }

    const std::string& Source()
    {
        return g_source;
    }

    namespace
    {
        double Decoded(const Track& t)
        {
            if (t.detached)
                return t.decodedEnd;
            return t.startSeconds + double(g_frames[t.context] - t.startFrames) * SAMPLES_PER_FRAME / t.rate;
        }

        // The context of `t` starts other audio. A sound decoded whole keeps playing from the title's
        // memory; a stream cut short was stopped.
        bool KeepAfterContextReuse(Track& t)
        {
            const Sound& sound = g_sounds[t.sound];
            double decoded = Decoded(t);
            bool whole = sound.seconds > 0.0 && decoded >= sound.seconds - 0.15;
            if (g_trace)
                Log("[subtitles] %s: context %u moves on (%s)", sound.key.c_str(), t.context, whole ? "decoded whole, still playing" : "stopped");
            if (!whole)
                return false;
            t.detached = true;
            t.decodedEnd = decoded;
            return true;
        }
    }

    void OnInputBuffer(uint32_t context, const uint8_t* bytes, uint32_t packets, uint32_t sampleRate, uint64_t frames)
    {
        if (!g_ready || context >= CONTEXTS || !bytes || packets == 0)
            return;
        uint64_t hash = Fnv(bytes, size_t(std::min(HASHED_PACKETS, packets)) * PACKET_BYTES);
        auto it = g_refs.find(hash);
        const Ref* ref = it != g_refs.end() && !it->second.ambiguous ? &it->second : nullptr;

        std::lock_guard lock(g_mutex);
        auto track = std::find_if(g_tracks.begin(), g_tracks.end(), [&](const Track& t) { return t.context == context && !t.detached; });
        if (track != g_tracks.end())
        {
            if (ref && track->sound == ref->sound && ref->block > 0)
                return;  // the next block of the sound being played
            if (!ref && it != g_refs.end())
                return;  // ambiguous (silence shared by several sounds): assume it continues
            if (!KeepAfterContextReuse(*track))
                g_tracks.erase(track);
        }
        if (!ref)
            return;
        // Every context gets a track, also the other streams of a multi-stream sound (so their next
        // blocks are known); CurrentText shows each sound once.
        bool follower = std::any_of(g_tracks.begin(), g_tracks.end(), [&](const Track& t) { return t.sound == ref->sound && !t.detached; });
        const Sound& sound = g_sounds[ref->sound];
        Track t{};
        t.sound = ref->sound;
        t.context = context;
        t.startFrames = frames;
        t.rate = sound.rate ? sound.rate : sampleRate;
        t.startSeconds = ref->block < int(sound.blockStart.size()) ? double(sound.blockStart[ref->block]) / t.rate : 0.0;
        t.startWall = Now();
        t.lastAudio = audio::SubmittedFrames();
        t.clock = t.startSeconds;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_lastAdvance[context] = now.QuadPart;
        g_frames[context] = frames;
        g_tracks.push_back(t);
        if (!follower)
        {
            std::vector<double>& starts = g_recentStarts[context];
            starts.erase(std::remove_if(starts.begin(), starts.end(), [&](double s) { return t.startWall - s > BULK_SECONDS; }), starts.end());
            starts.push_back(t.startWall);
            if (starts.size() >= BULK_COUNT)
            {
                // A row of sounds unpacked at load time: drop them (the ones decoded whole moved to
                // detached tracks on this context).
                size_t dropped = 0;
                for (auto it = g_tracks.begin(); it != g_tracks.end();)
                {
                    if (it->context == context && t.startWall - it->startWall <= BULK_SECONDS)
                        it = g_tracks.erase(it), dropped++;
                    else
                        ++it;
                }
                if (g_trace)
                    Log("[subtitles] context %u: %zu sounds in %.1f s, loaded rather than played (%s...)", context, dropped, BULK_SECONDS,
                        sound.key.c_str());
                return;
            }
        }
        if (!follower && g_trace)
            Log("[subtitles] %s starts on XMA context %u (block %d, at %.2f s; audio frame %llu)", sound.key.c_str(), context, ref->block,
                t.startSeconds, (unsigned long long)t.lastAudio);
    }

    void OnFrames(uint32_t context, uint64_t frames)
    {
        if (!g_ready || context >= CONTEXTS)
            return;
        if (g_frames[context].exchange(frames) != frames)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            g_lastAdvance[context] = now.QuadPart;
        }
    }

    std::wstring CurrentText(uint64_t& version, bool& gameplay)
    {
        if (!g_testText.empty())
        {
            version = 1;
            gameplay = false;
            return g_testText;
        }
        std::lock_guard lock(g_mutex);
        if (!g_tracks.empty())
        {
            const double now = Now();
            const uint64_t audioFrames = audio::SubmittedFrames();
            std::wstring text;
            bool fromClip = false;
            std::vector<int> shown;  // sounds already handled (multi-stream sounds have a track per stream)
            for (auto t = g_tracks.begin(); t != g_tracks.end();)
            {
                const Sound& sound = g_sounds[t->sound];
                double decoded = Decoded(*t);
                double idle = t->detached ? 0.0 : now - double(g_lastAdvance[t->context]) / double(g_frequency.QuadPart);
                double elapsed = double(audioFrames - t->lastAudio) * audio::FRAME_SAMPLES / audio::SAMPLE_RATE;
                t->lastAudio = audioFrames;
                if (idle <= PAUSE_SECONDS)
                    t->clock += elapsed;
                double end = sound.seconds > 0.0 ? sound.seconds : decoded;
                // Never ahead of what was decoded, except once the whole sound is: then the clock runs
                // past the end (with the output delay) and the track finishes.
                bool whole = decoded >= end - 0.15;
                t->clock = std::min(t->clock, whole ? end + g_offset + 1.0 : decoded);
                if (idle > FORGET_SECONDS || t->clock >= end + g_offset + 0.3)
                {
                    if (g_trace)
                        Log("[subtitles] %s ends (at %.2f s, idle %.1f s)", sound.key.c_str(), t->clock, idle);
                    t = g_tracks.erase(t);
                    continue;
                }
                if (std::find(shown.begin(), shown.end(), t->sound) != shown.end())
                {
                    ++t;
                    continue;
                }
                shown.push_back(t->sound);
                if (g_trace)
                {
                    static double lastTrace;
                    if (now - lastTrace > 0.5)
                    {
                        lastTrace = now;
                        Log("[subtitles] trace %s: at %.2f s, decoded %.2f s, idle %.2f s, audio frame %llu", sound.key.c_str(), t->clock,
                            decoded, idle, (unsigned long long)audioFrames);
                    }
                }
                // A clip decoded whole right away may be a load-time decode: shown once that's ruled out.
                bool pending = decoded >= end - 0.15 && now - t->startWall < HOLD_SECONDS;
                if (idle <= PAUSE_SECONDS && !pending)
                {
                    double at = t->clock - g_offset;
                    for (const Line& line : sound.lines)
                    {
                        if (at >= line.start && at < line.end)
                        {
                            if (!text.empty())
                                text += L'\n';
                            text += line.text;
                            fromClip |= sound.key.find('/') != std::string::npos;
                        }
                    }
                }
                ++t;
            }
            if (text != g_text || fromClip != g_gameplay)
            {
                g_text = std::move(text);
                g_gameplay = fromClip;
                g_version++;
                if (g_trace)
                {
                    std::string utf8(size_t(WideCharToMultiByte(CP_UTF8, 0, g_text.c_str(), int(g_text.size()), nullptr, 0, nullptr, nullptr)), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, g_text.c_str(), int(g_text.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
                    Log("[subtitles] text: %s", g_text.empty() ? "(none)" : utf8.c_str());
                }
            }
        }
        else if (!g_text.empty())
        {
            g_text.clear();
            g_version++;
        }
        version = g_version;
        gameplay = g_gameplay;
        return g_text;
    }
}
