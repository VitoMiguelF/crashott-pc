// Online multiplayer prototype: switches and the player list (mp.h).

#include "mp.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
    constexpr uint32_t GAME_DATABASE = 0x824A76A4;  // CGameDatabase: player actors at +16 / +20

    struct Config
    {
        bool enabled = false;
        mp::StreamMode stream = mp::StreamMode::Vanilla;
        bool trace = false;
        uint32_t traceMs = 1000;
    };

    const Config& Get()
    {
        static Config config = [] {
            Config c;
            const char* on = getenv("CRASH_MP");
            c.enabled = on && *on && strcmp(on, "0") != 0;
            if (const char* s = getenv("CRASH_MP_STREAM"))
            {
                if (!strcmp(s, "union"))
                    c.stream = mp::StreamMode::Union;
                else if (!strcmp(s, "all"))
                    c.stream = mp::StreamMode::All;
            }
            const char* trace = getenv("CRASH_MP_TRACE");
            c.trace = trace && *trace && strcmp(trace, "0") != 0;
            if (c.trace && atoi(trace) > 1)
                c.traceMs = uint32_t(atoi(trace));
            if (c.enabled)
                Log("[mp] prototype on: stream %s, trace %d", c.stream == mp::StreamMode::All ? "all"
                    : c.stream == mp::StreamMode::Union ? "union" : "vanilla", int(c.trace));
            return c;
        }();
        return config;
    }

    std::vector<uint32_t> MakeAnchors()
    {
        std::vector<uint32_t> out;
        const char* text = getenv("CRASH_MP_ANCHOR");
        while (text && *text)
        {
            float p[3];
            if (sscanf(text, "%f,%f,%f", &p[0], &p[1], &p[2]) != 3)
                break;
            uint32_t guest = GuestAlloc(16);
            for (int i = 0; i < 3; ++i)
            {
                uint32_t bits;
                memcpy(&bits, &p[i], 4);
                StoreBE32(guest + i * 4, bits);
            }
            out.push_back(guest);
            Log("[mp] anchor %zu at (%.1f, %.1f, %.1f)", out.size(), p[0], p[1], p[2]);
            const char* next = strchr(text, ';');
            text = next ? next + 1 : nullptr;
        }
        return out;
    }

    std::mutex g_mutex;
    std::vector<uint32_t> g_extra;  // Crashes added by the prototype

    std::atomic<bool> g_session{ false };     // a MULTIPLAYER session is open (mp_lobby.cpp)
    std::atomic<uint32_t> g_sessionPlayers{ 0 };
}

namespace mp
{
    bool Enabled() { return Get().enabled || g_session.load(std::memory_order_relaxed); }

    void SetSession(bool open, uint32_t players)
    {
        g_sessionPlayers = std::clamp<uint32_t>(players, 1, 4);
        if (g_session.exchange(open) != open)
            Log("[mp] session %s", open ? "open: the prototype is on" : "closed");
    }
    StreamMode Stream() { return Get().stream; }
    bool Trace() { return Get().trace; }

    uint32_t Players()
    {
        if (g_session.load(std::memory_order_relaxed))
            return g_sessionPlayers.load(std::memory_order_relaxed);
        static uint32_t players = [] {
            const char* text = getenv("CRASH_MP_PLAYERS");
            int n = text ? atoi(text) : 2;
            return uint32_t(n < 2 ? 2 : n > 4 ? 4 : n);
        }();
        return players;
    }
    uint32_t TraceIntervalMs() { return Get().traceMs; }

    uint64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    std::vector<uint32_t> PlayerActors()
    {
        std::vector<uint32_t> out;
        for (uint32_t slot : {16u, 20u})
            if (uint32_t actor = LoadBE32(GAME_DATABASE + slot))
                out.push_back(actor);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t actor : g_extra)
            if (std::find(out.begin(), out.end(), actor) == out.end())
                out.push_back(actor);
        return out;
    }

    bool InLevel()
    {
        uint32_t sm = StreamManager();
        return sm && LoadBE32(sm + 636) && LoadBE32(GAME_DATABASE + 16);
    }

    void AddPlayer(uint32_t actor)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (actor && std::find(g_extra.begin(), g_extra.end(), actor) == g_extra.end())
            g_extra.push_back(actor);
    }

    const std::vector<uint32_t>& Anchors()
    {
        static std::vector<uint32_t> anchors = MakeAnchors();
        return anchors;
    }

    void Tick(PPCContext& ctx, uint8_t* base)
    {
        PlayersTick(ctx, base);
        NetTick(ctx, base);
    }
}
