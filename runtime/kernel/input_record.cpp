// Recording and replay of controller 0 on the game tick clock (see input_record.h for the clock).
//
// Recording. From the first controller read on, every change of what the game reads from
// controller 0 is kept in memory: button presses (start tick and length), both sticks and both
// triggers in hundredths (a stick is written again when an axis moves 0.02, reaches the rest
// position or full deflection; |value| < 0.05 counts as rest), plus a SYNC at the end of every load.
// F5 starts a recording (or CRASH_RECORD_INPUT=1 from boot); F5 again, or closing the window,
// stops it. The profile written to CRASH_RECORD_DIR\<name>.toml replays from boot: everything up to
// F5 (the way to the recorded moment), the recorded part, screenshots at F5, at each F2 and at the
// stop, and END a second later. The user data as it was at boot (content\ and settings.toml) goes
// to CRASH_RECORD_DIR\<name>\userdata, which the profile copies (userdata_from). The file is
// rewritten every 2 s while recording, so a run that is killed still leaves a usable profile.
//
// Load detection. default.rcf holds the level and menu data; the game reads it in bursts of
// megabytes when something loads and in small pieces otherwise (kernel/file_imports.cpp reports
// every read). A burst of >= 1 MB followed by 1 s without default.rcf reads ends a load: the SYNC
// keeps the tick of the burst's last read and its anchor, the offset of that read and how many
// times the game had read that offset by then.
//
// Replay (CRASH_REPLAY=<file>, written by tools/run_game.py from a profile's `replay`). One entry
// per line, "tick KIND args": BTN name hold_ticks, LS/RS x y, LT/RT v (floats, -1..1 / 0..1),
// SYNC offset_hex count, WAIT text, SHOT, END. Entries are applied on the tick they name plus the
// timeline's shift. A SYNC holds the timeline until the anchor offset has been read that many
// times and default.rcf has been quiet for 5 ticks, then moves it so the SYNC lands on the replay's
// own last read (later when the load took longer, earlier when it was faster). A WAIT holds it
// until a log line containing the text has appeared (in order: each WAIT matches a line after the
// previous WAIT's), and moves it by the time spent waiting. Neither holds longer than 60 s: then the
// replay logs "timed out" and goes on. Buttons are pressed for their length from the tick they are
// applied on, so a shift never cuts a press.
//
// Events (settings/direct_boot.h: main_menu, hub, level_ready, gameplay_ready) are recorded too and
// written as [t, "EVENT", "name"] (replay line "tick EVENT name"). The replay holds the timeline at
// an EVENT until the event's log line has appeared, like a WAIT, but then moves it so the EVENT
// lands on the tick the event happened (earlier too, like a SYNC): what follows it keeps its
// distance to the game's own milestone.

#include "input.h"
#include "input_record.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr double TICKS_PER_SECOND = 30.0;
    constexpr int64_t LOAD_QUIET_TICKS = 30;           // recording: no default.rcf reads for 1 s ends a load
    constexpr uint64_t LOAD_MIN_BYTES = 1 << 20;       // smaller bursts are streaming, not loads
    constexpr int64_t SYNC_QUIET_TICKS = 5;            // replay: quiet after the anchor read
    constexpr int64_t HOLD_LIMIT_TICKS = 60 * 30;      // a SYNC/WAIT lets the timeline go after 60 s
    constexpr int64_t DEFAULT_HOLD_TICKS = 6;          // 0.2 s, the length a button without one gets
    constexpr int64_t WRITE_EVERY_TICKS = 60;          // the profile is rewritten every 2 s while recording
    constexpr int64_t END_AFTER_TICKS = 30;            // END one second after the stop
    constexpr int ANALOG_STEP = 2;                     // hundredths
    constexpr int ANALOG_REST = 5;
    constexpr uint64_t SNAPSHOT_LIMIT = 64ull << 20;

    // run.toml names (the same as xam_input_imports.cpp, plus the stick clicks).
    constexpr std::pair<const char*, WORD> BUTTONS[] = {
        { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
        { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "UP", XINPUT_GAMEPAD_DPAD_UP },
        { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
        { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { "L3", XINPUT_GAMEPAD_LEFT_THUMB }, { "R3", XINPUT_GAMEPAD_RIGHT_THUMB },
    };
    constexpr size_t BUTTON_COUNT = std::size(BUTTONS);

    enum class Kind { Sync, Wait, Button, LeftStick, RightStick, LeftTrigger, RightTrigger, Shot, End, Event };

    // The test events a recording keeps as anchors (the others don't help a replay re-align).
    constexpr const char* ANCHOR_EVENTS[] = { "main_menu", "hub", "level_ready", "gameplay_ready" };

    struct Entry
    {
        int64_t tick = 0;
        Kind kind = Kind::Button;
        size_t button = 0;     // BUTTONS index
        int64_t hold = 0;      // ticks
        int x = 0, y = 0;      // hundredths (a trigger uses x)
        uint64_t offset = 0;   // SYNC anchor
        uint32_t count = 0;
        std::string text;      // WAIT, EVENT (its name); for a SHOT, a comment in the profile
    };

    // Polls so far. Poll(k) runs with g_tick == k + 1 after its increment; a read between polls k
    // and k + 1 is stamped k + 1, in the recording and in the replay alike.
    std::atomic<int64_t> g_tick{ 0 };

    struct Loads
    {
        std::mutex mutex;
        std::unordered_map<uint64_t, uint32_t> reads;  // default.rcf offset -> times read
        int64_t lastTick = INT64_MIN / 2;
        uint64_t lastOffset = 0;
        uint32_t lastCount = 0;
        uint64_t burstBytes = 0;
    };
    Loads g_loads;

    std::string Seconds(int64_t ticks)
    {
        char text[32];
        snprintf(text, sizeof(text), "%.2f", double(ticks) / TICKS_PER_SECOND);
        return text;
    }

    std::string Hundredths(int value)
    {
        char text[16];
        snprintf(text, sizeof(text), "%.2f", value / 100.0);
        return text;
    }

    int StickHundredths(SHORT value)
    {
        int q = int(std::lround(double(value) / 327.67));
        q = std::clamp(q, -100, 100);
        return std::abs(q) < ANALOG_REST ? 0 : q;
    }

    int TriggerHundredths(BYTE value)
    {
        int q = int(std::lround(double(value) / 2.55));
        return q < ANALOG_REST ? 0 : std::min(q, 100);
    }

    bool AnalogMoved(int last, int now)
    {
        return now != last && (std::abs(now - last) >= ANALOG_STEP || now == 0 || std::abs(now) == 100);
    }

    std::string LocalTime(const char* format)
    {
        time_t now = time(nullptr);
        tm local{};
        localtime_s(&local, &now);
        char text[64];
        strftime(text, sizeof(text), format, &local);
        return text;
    }

    fs::path RecordDir()
    {
        if (const char* dir = getenv("CRASH_RECORD_DIR"); dir && *dir)
            return fs::path(dir);
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::path folder = fs::path(exe).parent_path();
        // build\CrashRecomp.exe in the project: next to config\run.toml.
        if (fs::exists(folder.parent_path() / "config" / "run.toml"))
            return folder.parent_path() / "config" / "recorded";
        return folder / "recorded";
    }

    bool EnvFlag(const char* name)
    {
        const char* value = getenv(name);
        return value && *value && strcmp(value, "0") != 0;
    }

    // Replay -------------------------------------------------------------------------------------

    struct Replay
    {
        std::mutex mutex;
        std::vector<Entry> entries;
        size_t next = 0;
        int64_t shift = 0;
        bool active = false;
        int64_t lastTick = 0;
        WORD buttons = 0;
        int64_t release[BUTTON_COUNT]{};
        int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        int64_t holdSince = -1;
        const char* waiting = nullptr;
        int syncs = 0;

        // WAIT entries in timeline order and the tick their line appeared (-1 = not yet).
        std::mutex waitMutex;
        std::vector<std::string> waitTexts;
        std::vector<int64_t> waitSeen;
        size_t nextWaitToMatch = 0;
        std::vector<size_t> waitIndex;  // entries index -> waitTexts index

        Replay()
        {
            const char* path = getenv("CRASH_REPLAY");
            if (!path || !*path)
                return;
            std::ifstream file(path);
            if (!file)
            {
                Log("[input] CRASH_REPLAY: cannot open %s", path);
                return;
            }
            std::string line;
            int lineNumber = 0;
            while (std::getline(file, line))
            {
                lineNumber++;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream in(line);
                Entry e;
                std::string kind;
                if (!(in >> e.tick >> kind))
                {
                    Log("[input] CRASH_REPLAY line %d ignored: %s", lineNumber, line.c_str());
                    continue;
                }
                bool ok = true;
                if (kind == "BTN")
                {
                    std::string name;
                    in >> name >> e.hold;
                    ok = false;
                    for (size_t i = 0; i < BUTTON_COUNT; i++)
                    {
                        if (_stricmp(BUTTONS[i].first, name.c_str()) == 0)
                            e.button = i, ok = true;
                    }
                    e.hold = std::max<int64_t>(e.hold, 1);
                }
                else if (kind == "LS" || kind == "RS" || kind == "LT" || kind == "RT")
                {
                    double x = 0.0, y = 0.0;
                    in >> x;
                    if (kind[1] == 'S')
                        in >> y;
                    e.kind = kind == "LS" ? Kind::LeftStick : kind == "RS" ? Kind::RightStick : kind == "LT" ? Kind::LeftTrigger : Kind::RightTrigger;
                    e.x = std::clamp(int(std::lround(x * 100.0)), kind[1] == 'S' ? -100 : 0, 100);
                    e.y = std::clamp(int(std::lround(y * 100.0)), -100, 100);
                    ok = !in.fail();
                }
                else if (kind == "SYNC")
                {
                    std::string offset;
                    in >> offset >> e.count;
                    e.kind = Kind::Sync;
                    e.offset = strtoull(offset.c_str(), nullptr, 16);
                    ok = !in.fail() && e.count > 0;
                }
                else if (kind == "WAIT")
                {
                    std::getline(in >> std::ws, e.text);
                    e.kind = Kind::Wait;
                    ok = !e.text.empty();
                }
                else if (kind == "EVENT")
                {
                    std::string name;
                    in >> name;
                    e.kind = Kind::Event;
                    e.text = "[test] event " + name + " (";  // direct_boot.cpp's line
                    ok = !name.empty();
                }
                else if (kind == "SHOT")
                    e.kind = Kind::Shot;
                else if (kind == "END")
                    e.kind = Kind::End;
                else
                    ok = false;
                if (!ok)
                {
                    Log("[input] CRASH_REPLAY line %d ignored: %s", lineNumber, line.c_str());
                    continue;
                }
                entries.push_back(std::move(e));
            }
            std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tick < b.tick; });
            waitIndex.assign(entries.size(), 0);
            for (size_t i = 0; i < entries.size(); i++)
            {
                if (entries[i].kind == Kind::Wait || entries[i].kind == Kind::Event)
                {
                    waitIndex[i] = waitTexts.size();
                    waitTexts.push_back(entries[i].text);
                    waitSeen.push_back(-1);
                }
            }
            active = !entries.empty();
            int syncCount = int(std::count_if(entries.begin(), entries.end(), [](const Entry& e) { return e.kind == Kind::Sync; }));
            Log("[input] replay: %zu entries from %s (%d SYNC, %zu WAIT/EVENT), %s s of game time", entries.size(), path, syncCount,
                waitTexts.size(), entries.empty() ? "0" : Seconds(entries.back().tick).c_str());
        }

        // The diagnostics log observer: the next WAIT's text in a line.
        void OnLogLine(const char* line)
        {
            if (strncmp(line, "[input]", 7) == 0)
                return;
            std::lock_guard lock(waitMutex);
            if (nextWaitToMatch < waitTexts.size() && strstr(line, waitTexts[nextWaitToMatch].c_str()))
                waitSeen[nextWaitToMatch++] = g_tick.load();
        }

        void Hold(int64_t tick, const char* what)
        {
            if (holdSince < 0)
            {
                holdSince = tick;
                Log("[input] replay t=%s s: waiting for %s", Seconds(tick - shift).c_str(), what);
            }
        }

        // Applies every entry due on tick `tick`; false while a SYNC/WAIT holds the timeline.
        void Step(int64_t tick)
        {
            lastTick = tick;
            for (size_t i = 0; i < BUTTON_COUNT; i++)
            {
                if ((buttons & BUTTONS[i].second) && release[i] <= tick)
                    buttons &= ~BUTTONS[i].second;
            }
            while (next < entries.size() && entries[next].tick + shift <= tick)
            {
                const Entry& e = entries[next];
                switch (e.kind)
                {
                case Kind::Button:
                    buttons |= BUTTONS[e.button].second;
                    release[e.button] = tick + e.hold;
                    break;
                case Kind::LeftStick: lx = e.x, ly = e.y; break;
                case Kind::RightStick: rx = e.x, ry = e.y; break;
                case Kind::LeftTrigger: lt = e.x; break;
                case Kind::RightTrigger: rt = e.x; break;
                case Kind::Shot: ShotRequested(); break;
                case Kind::End:
                    Log("[input] replay finished at t=%s s (tick %lld, timeline %+.2f s, %d sync(s)); closing the game",
                        Seconds(e.tick).c_str(), (long long)tick, shift / TICKS_PER_SECOND, syncs);
                    fflush(stdout);
                    ExitProcess(0);
                case Kind::Sync:
                {
                    bool loaded;
                    int64_t lastRead;
                    {
                        std::lock_guard lock(g_loads.mutex);
                        auto found = g_loads.reads.find(e.offset);
                        loaded = found != g_loads.reads.end() && found->second >= e.count && tick + 1 - g_loads.lastTick >= SYNC_QUIET_TICKS;
                        lastRead = g_loads.lastTick;
                    }
                    if (!loaded && (holdSince < 0 || tick - holdSince < HOLD_LIMIT_TICKS))
                    {
                        Hold(tick, "the end of a load");
                        waiting = "CARREGAMENTO";
                        return;
                    }
                    int64_t before = shift;
                    if (loaded)
                    {
                        shift = lastRead - e.tick;
                        syncs++;
                        Log("[input] replay sync %d at t=%s s: load ended %+.2f s from the recording (tick %lld, timeline %+.2f s)",
                            syncs, Seconds(e.tick).c_str(), (shift - before) / TICKS_PER_SECOND, (long long)lastRead, shift / TICKS_PER_SECOND);
                    }
                    else
                    {
                        Log("[input] replay sync at t=%s s timed out: default.rcf offset 0x%llX was not read %u time(s); going on",
                            Seconds(e.tick).c_str(), (unsigned long long)e.offset, e.count);
                    }
                    holdSince = -1;
                    waiting = nullptr;
                    break;
                }
                case Kind::Event:
                {
                    int64_t seen;
                    {
                        std::lock_guard lock(waitMutex);
                        seen = waitSeen[waitIndex[next]];
                    }
                    std::string name = e.text.substr(13, e.text.size() - 15);
                    if (seen < 0 && (holdSince < 0 || tick - holdSince < HOLD_LIMIT_TICKS))
                    {
                        Hold(tick, ("the event " + name).c_str());
                        waiting = "EVENTO";
                        return;
                    }
                    int64_t before = shift;
                    if (seen >= 0)
                    {
                        shift = seen - e.tick;
                        Log("[input] replay event %s at t=%s s: %+.2f s from the recording (tick %lld, timeline %+.2f s)", name.c_str(),
                            Seconds(e.tick).c_str(), (shift - before) / TICKS_PER_SECOND, (long long)seen, shift / TICKS_PER_SECOND);
                    }
                    else
                    {
                        Log("[input] replay event %s at t=%s s timed out; going on", name.c_str(), Seconds(e.tick).c_str());
                    }
                    holdSince = -1;
                    waiting = nullptr;
                    break;
                }
                case Kind::Wait:
                {
                    int64_t seen;
                    {
                        std::lock_guard lock(waitMutex);
                        seen = waitSeen[waitIndex[next]];
                    }
                    if (seen < 0 && (holdSince < 0 || tick - holdSince < HOLD_LIMIT_TICKS))
                    {
                        Hold(tick, ("the log line \"" + e.text + "\"").c_str());
                        waiting = "LOG";
                        return;
                    }
                    if (holdSince >= 0)
                    {
                        shift += tick - holdSince;
                        Log("[input] replay t=%s s: \"%s\" %s after %.2f s (timeline %+.2f s)", Seconds(e.tick).c_str(), e.text.c_str(),
                            seen < 0 ? "timed out" : "appeared", (tick - holdSince) / TICKS_PER_SECOND, shift / TICKS_PER_SECOND);
                    }
                    holdSince = -1;
                    waiting = nullptr;
                    break;
                }
                }
                next++;
            }
        }

        void Add(XINPUT_STATE& state) const
        {
            if (!active)
                return;
            XINPUT_GAMEPAD& pad = state.Gamepad;
            pad.wButtons |= buttons;
            // The replay wins over a physical stick/trigger at rest.
            if (lx || ly)
                pad.sThumbLX = SHORT(std::lround(lx * 327.67)), pad.sThumbLY = SHORT(std::lround(ly * 327.67));
            if (rx || ry)
                pad.sThumbRX = SHORT(std::lround(rx * 327.67)), pad.sThumbRY = SHORT(std::lround(ry * 327.67));
            if (lt)
                pad.bLeftTrigger = BYTE(std::max<long>(pad.bLeftTrigger, std::lround(lt * 2.55)));
            if (rt)
                pad.bRightTrigger = BYTE(std::max<long>(pad.bRightTrigger, std::lround(rt * 2.55)));
        }

        static void ShotRequested();
    };

    std::atomic<uint32_t> g_shots{ 0 };

    void Replay::ShotRequested()
    {
        g_shots++;
    }

    Replay& TheReplay()
    {
        static Replay replay;
        return replay;
    }

    // Recording ----------------------------------------------------------------------------------

    struct Recorder
    {
        std::mutex mutex;
        // Everything since boot; a profile takes what it needs.
        std::vector<Entry> entries;
        WORD lastButtons = 0;
        int64_t down[BUTTON_COUNT]{};
        int lastL[2]{}, lastR[2]{}, lastLT = 0, lastRT = 0;
        int64_t tick = 0;

        // The user data at boot.
        std::vector<std::pair<fs::path, std::string>> snapshot;
        bool snapshotTaken = false;

        bool active = false;
        int64_t start = 0;
        int64_t lastWrite = 0;
        std::string name, startedAt;
        std::vector<int64_t> marks;
        fs::path dir;

        void TakeSnapshot()
        {
            snapshotTaken = true;
            const fs::path& root = vfs::UserDataDir();
            uint64_t total = 0;
            std::error_code error;
            std::vector<fs::path> files;
            if (fs::is_regular_file(root / "settings.toml", error))
                files.push_back(root / "settings.toml");
            if (fs::is_directory(root / "content", error))
            {
                for (auto it = fs::recursive_directory_iterator(root / "content", error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
                {
                    if (it->is_regular_file(error))
                        files.push_back(it->path());
                }
            }
            for (const fs::path& file : files)
            {
                std::ifstream in(file, std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                total += data.size();
                if (total > SNAPSHOT_LIMIT)
                {
                    Log("[input] user data over %llu MB: recordings will start from the saves of the replay run", (unsigned long long)(SNAPSHOT_LIMIT >> 20));
                    snapshot.clear();
                    return;
                }
                snapshot.emplace_back(fs::relative(file, root, error), std::move(data));
            }
        }

        void WriteSnapshot()
        {
            fs::path target = dir / name / "userdata";
            std::error_code error;
            fs::create_directories(target, error);
            for (const auto& [relative, data] : snapshot)
            {
                fs::create_directories((target / relative).parent_path(), error);
                std::ofstream out(target / relative, std::ios::binary | std::ios::trunc);
                out.write(data.data(), std::streamsize(data.size()));
            }
        }

        void Push(Entry e)
        {
            entries.push_back(std::move(e));
        }

        // One poll's state (k = the poll's tick).
        void Sample(const XINPUT_GAMEPAD& pad, int64_t k)
        {
            tick = k;
            WORD changed = pad.wButtons ^ lastButtons;
            for (size_t i = 0; i < BUTTON_COUNT && changed; i++)
            {
                WORD bit = BUTTONS[i].second;
                if (!(changed & bit))
                    continue;
                if (pad.wButtons & bit)
                    down[i] = k;
                else
                    Push({ down[i], Kind::Button, i, k - down[i] });
            }
            lastButtons = pad.wButtons;

            int l[2] = { StickHundredths(pad.sThumbLX), StickHundredths(pad.sThumbLY) };
            int r[2] = { StickHundredths(pad.sThumbRX), StickHundredths(pad.sThumbRY) };
            if (AnalogMoved(lastL[0], l[0]) || AnalogMoved(lastL[1], l[1]))
            {
                Push({ k, Kind::LeftStick, 0, 0, l[0], l[1] });
                lastL[0] = l[0], lastL[1] = l[1];
            }
            if (AnalogMoved(lastR[0], r[0]) || AnalogMoved(lastR[1], r[1]))
            {
                Push({ k, Kind::RightStick, 0, 0, r[0], r[1] });
                lastR[0] = r[0], lastR[1] = r[1];
            }
            int lt = TriggerHundredths(pad.bLeftTrigger), rt = TriggerHundredths(pad.bRightTrigger);
            if (AnalogMoved(lastLT, lt))
                Push({ k, Kind::LeftTrigger, 0, 0, lt }), lastLT = lt;
            if (AnalogMoved(lastRT, rt))
                Push({ k, Kind::RightTrigger, 0, 0, rt }), lastRT = rt;

            {
                std::lock_guard lock(g_loads.mutex);
                if (g_loads.burstBytes && k + 1 - g_loads.lastTick >= LOAD_QUIET_TICKS)
                {
                    if (g_loads.burstBytes >= LOAD_MIN_BYTES)
                    {
                        Entry sync{ g_loads.lastTick, Kind::Sync };
                        sync.offset = g_loads.lastOffset;
                        sync.count = g_loads.lastCount;
                        Push(sync);
                    }
                    g_loads.burstBytes = 0;
                }
            }

            if (active && k - lastWrite >= WRITE_EVERY_TICKS)
            {
                lastWrite = k;
                Write(k, false);
            }
        }

        void Start(int64_t k)
        {
            active = true;
            start = k;
            lastWrite = k;
            marks.clear();
            name = "gravado_" + LocalTime("%Y%m%d_%H%M%S");
            startedAt = LocalTime("%Y-%m-%d %H:%M:%S");
            dir = RecordDir();
            g_shots++;  // the recording run's own screenshot of the moments the replay captures
            std::error_code error;
            fs::create_directories(dir, error);
            WriteSnapshot();
            Write(k, false);
            Log("[input] recording started (F5) at t=%s s: %s", Seconds(k).c_str(), (dir / (name + ".toml")).string().c_str());
        }

        void Stop(int64_t k)
        {
            active = false;
            g_shots++;
            Write(k, true);
            Log("[input] recording stopped at t=%s s (%s s recorded): python tools/run_game.py %s", Seconds(k).c_str(),
                Seconds(k - start).c_str(), name.c_str());
        }

        // The profile for a recording from `start` to `end` (ticks).
        void Write(int64_t end, bool final)
        {
            std::vector<Entry> all;
            for (const Entry& e : entries)
            {
                if (e.tick <= end)
                    all.push_back(e);
            }
            // Buttons still down at the stop.
            for (size_t i = 0; i < BUTTON_COUNT; i++)
            {
                if (lastButtons & BUTTONS[i].second)
                    all.push_back({ down[i], Kind::Button, i, std::max<int64_t>(end - down[i], 1) });
            }
            Entry shot{ start, Kind::Shot };
            shot.text = "F5: inicio da gravacao";
            all.push_back(shot);
            for (int64_t mark : marks)
            {
                shot.tick = mark;
                shot.text = "F2";
                all.push_back(shot);
            }
            shot.tick = end;
            shot.text = "fim da gravacao";
            all.push_back(shot);
            all.push_back({ end + END_AFTER_TICKS, Kind::End });
            // By tick, a SYNC or EVENT first: what follows it on the same tick is relative to it.
            auto anchor = [](const Entry& e) { return e.kind == Kind::Sync || e.kind == Kind::Event; };
            std::stable_sort(all.begin(), all.end(), [&](const Entry& a, const Entry& b) {
                return a.tick != b.tick ? a.tick < b.tick : anchor(a) > anchor(b);
            });
            // A SYNC or EVENT matters only when something follows it before the next one.
            std::vector<Entry> kept;
            for (size_t i = 0; i < all.size(); i++)
            {
                if (anchor(all[i]) && (i + 1 == all.size() || anchor(all[i + 1])))
                    continue;
                kept.push_back(all[i]);
            }

            std::string text;
            text += "# CrashRecomp: controle 0 gravado com F5 em " + startedAt + ".\n";
            text += "# Roda do boot com:  python tools/run_game.py " + name + "\n";
            text += "#\n";
            text += "# Tempo: segundos do jogo (leituras do controle / 30) desde a primeira leitura, nao do relogio.\n";
            text += "# [t, \"BOTAO\", s] aperta por s segundos (sem s: 0.2); [t, \"LS\"/\"RS\", x, y] analogico (-1..1,\n";
            text += "# fica ate a proxima entrada); [t, \"LT\"/\"RT\", v] gatilho (0..1); [t, \"SYNC\", \"offset#n\"] fim de\n";
            text += "# um carregamento (o replay espera e realinha o tempo); [t, \"WAIT\", \"texto\"] espera uma linha do\n";
            text += "# log; [t, \"EVENT\", \"nome\"] espera um evento do jogo (gameplay_ready...); [t, \"SHOT\"] captura;\n";
            text += "# [t, \"END\"] fecha o jogo. Veja README, \"Gravando um perfil jogando\".\n";
            text += "[profiles." + name + "]\n";
            char line[512];
            const char* source = getenv("CRASH_PROFILE");
            snprintf(line, sizeof(line), "description = \"Gravado com F5 em %s%s%s: %s s a partir de %s s do jogo%s\"\n", startedAt.c_str(),
                     source ? " no perfil " : "", source ? source : "", Seconds(end - start).c_str(), Seconds(start).c_str(),
                     final ? "" : " (gravacao interrompida)");
            text += line;
            text += "test = false\n";
            // Wall clock: about the game time, plus room for slower loads.
            snprintf(line, sizeof(line), "timeout = %lld\n", (long long)(end / TICKS_PER_SECOND + 120));
            text += line;
            text += EnvFlag("CRASH_NOAUDIO") ? "audio = false\n" : "audio = true\n";
            text += "userdata_mode = \"copy\"\n";
            if (!snapshot.empty())
                text += "userdata_from = \"" + name + "/userdata\"   # relativo a este arquivo: content\\ e settings.toml do boot\n";
            if (const char* overrides = getenv("CRASH_SETTINGS"); overrides && *overrides)
                text += std::string("env = { CRASH_SETTINGS = '") + overrides + "' }\n";
            // The boot the recording started with (the replay skips the same menus).
            if (const char* boot = getenv("CRASH_BOOT"); boot && *boot)
                text += std::string("boot = '") + boot + "'\n";
            else if (EnvFlag("CRASH_SKIP_INTROS"))
                text += "skip_intros = true\n";
            text += "replay = [\n";

            std::string row;
            auto flush = [&] {
                if (!row.empty())
                    text += "    " + row + "\n";
                row.clear();
            };
            auto item = [&](const std::string& value) {
                if (!row.empty() && row.size() + value.size() > 96)
                    flush();
                row += (row.empty() ? "" : " ") + value + ",";
            };
            bool startWritten = false;
            for (const Entry& e : kept)
            {
                if (!startWritten && e.tick >= start)
                {
                    flush();
                    text += "    # --- F5: inicio da gravacao (" + Seconds(start) + " s) ---\n";
                    startWritten = true;
                }
                std::string t = Seconds(e.tick);
                switch (e.kind)
                {
                case Kind::Button:
                    item("[" + t + ", \"" + BUTTONS[e.button].first + "\"" + (e.hold == DEFAULT_HOLD_TICKS ? "" : ", " + Seconds(e.hold)) + "]");
                    break;
                case Kind::LeftStick:
                case Kind::RightStick:
                    item("[" + t + (e.kind == Kind::LeftStick ? ", \"LS\", " : ", \"RS\", ") + Hundredths(e.x) + ", " + Hundredths(e.y) + "]");
                    break;
                case Kind::LeftTrigger:
                case Kind::RightTrigger:
                    item("[" + t + (e.kind == Kind::LeftTrigger ? ", \"LT\", " : ", \"RT\", ") + Hundredths(e.x) + "]");
                    break;
                case Kind::Sync:
                    flush();
                    snprintf(line, sizeof(line), "    [%s, \"SYNC\", \"0x%llX#%u\"],   # fim de carregamento\n", t.c_str(), (unsigned long long)e.offset, e.count);
                    text += line;
                    break;
                case Kind::Shot:
                    flush();
                    text += "    [" + t + ", \"SHOT\"],   # " + e.text + "\n";
                    break;
                case Kind::End:
                    flush();
                    text += "    [" + t + ", \"END\"],\n";
                    break;
                case Kind::Event:
                    flush();
                    text += "    [" + t + ", \"EVENT\", \"" + e.text + "\"],   # evento do jogo\n";
                    break;
                case Kind::Wait:
                    break;
                }
            }
            flush();
            text += "]\n";
            text += "expect = { exit = [\"ok\"], log_absent = ['\\[fatal\\]', '\\[crash\\]', 'replay sync .* timed out'], log_contains = ['\\[input\\] replay finished'] }\n";

            fs::path path = dir / (name + ".toml");
            fs::path temporary = path;
            temporary += ".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(text.data(), std::streamsize(text.size()));
            }
            MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };

    Recorder& TheRecorder()
    {
        static Recorder recorder;
        return recorder;
    }

    void OnLogLine(const char* line)
    {
        TheReplay().OnLogLine(line);
    }

    // F5 and F2 arrive on the window/presenter threads and take effect on the next game tick, the
    // way a replay's SHOT does, so the recording run's screenshots match the replay's.
    std::atomic<uint32_t> g_toggleRequests{ 0 }, g_markRequests{ 0 };
}

namespace input
{
    void NoteGameRead(uint64_t offset, uint64_t bytes, bool level)
    {
        if (!level || !bytes)
            return;
        std::lock_guard lock(g_loads.mutex);
        g_loads.lastTick = g_tick.load();
        g_loads.lastOffset = offset;
        g_loads.lastCount = ++g_loads.reads[offset];
        g_loads.burstBytes += bytes;
    }

    int64_t GameTick()
    {
        return g_tick.load();
    }

    void NoteEvent(const char* name)
    {
        if (std::none_of(std::begin(ANCHOR_EVENTS), std::end(ANCHOR_EVENTS), [&](const char* anchor) { return strcmp(anchor, name) == 0; }))
            return;
        Recorder& recorder = TheRecorder();
        std::lock_guard lock(recorder.mutex);
        Entry e{ g_tick.load(), Kind::Event };
        e.text = name;
        recorder.Push(std::move(e));
    }

    bool Recording()
    {
        Recorder& recorder = TheRecorder();
        std::lock_guard lock(recorder.mutex);
        return recorder.active;
    }

    const char* ToggleRecording()
    {
        bool stopping = Recording() != (g_toggleRequests.fetch_add(1) % 2 == 1);
        return stopping ? "GRAVACAO SALVA EM CONFIG/RECORDED" : "GRAVANDO (F5 PARA PARAR)";
    }

    void StopRecording()
    {
        Recorder& recorder = TheRecorder();
        std::lock_guard lock(recorder.mutex);
        if (recorder.active)
            recorder.Stop(std::max<int64_t>(g_tick.load() - 1, 0));
    }

    void RecordMark(uint64_t)
    {
        g_markRequests++;
    }

    RecordStatus Status()
    {
        RecordStatus status{};
        {
            Recorder& recorder = TheRecorder();
            std::lock_guard lock(recorder.mutex);
            status.recording = recorder.active;
            status.recordSeconds = recorder.active ? double(recorder.tick - recorder.start) / TICKS_PER_SECOND : 0.0;
        }
        Replay& replay = TheReplay();
        std::lock_guard lock(replay.mutex);
        status.replaying = replay.active;
        status.replaySeconds = double(replay.lastTick - replay.shift) / TICKS_PER_SECOND;
        status.shiftSeconds = double(replay.shift) / TICKS_PER_SECOND;
        status.waiting = replay.waiting;
        return status;
    }

    void RequestShot()
    {
        g_shots++;
    }

    bool ConsumeShotRequest()
    {
        uint32_t shots = g_shots.load();
        while (shots && !g_shots.compare_exchange_weak(shots, shots - 1))
        {
        }
        return shots != 0;
    }
}

namespace input::record
{
    void Poll(XINPUT_STATE& state)
    {
        int64_t k = g_tick.fetch_add(1);
        Replay& replay = TheReplay();
        if (k == 0)
            SetLogObserver(OnLogLine);
        {
            std::lock_guard lock(replay.mutex);
            if (replay.active)
            {
                replay.Step(k);
                replay.Add(state);
            }
        }
        Recorder& recorder = TheRecorder();
        std::lock_guard lock(recorder.mutex);
        if (!recorder.snapshotTaken)
        {
            recorder.TakeSnapshot();
            if (EnvFlag("CRASH_RECORD_INPUT"))
                recorder.Start(k);
        }
        for (uint32_t toggles = g_toggleRequests.exchange(0); toggles; toggles--)
            recorder.active ? recorder.Stop(k) : recorder.Start(k);
        for (uint32_t marks = g_markRequests.exchange(0); marks; marks--)
        {
            if (recorder.active)
            {
                recorder.marks.push_back(k);
                g_shots++;
            }
        }
        recorder.Sample(state.Gamepad, k);
    }

    void Apply(XINPUT_STATE& state)
    {
        Replay& replay = TheReplay();
        std::lock_guard lock(replay.mutex);
        replay.Add(state);
    }
}
