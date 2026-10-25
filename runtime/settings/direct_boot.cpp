// Skipping the intros, booting straight into a level or a save, and the game's milestones as
// events (direct_boot.h; README "Boot direto e eventos", runtime/NOTES.md "Boot direto").
//
// The frontend is a fight tree (fighttrees\Frontend.bfig): nodes (screens) and exits numbered
// together in file order, every node's conditions native code (sub_820F9BA8 returns the exit to
// take, -1 = stay; frontend_hooks.cpp). tools/frontend_tree.py prints the whole tree. Booting
// takes the tree's own exits at the right screens, so every snippet the game runs on them (the
// save loading, the music, the HUB setup) runs as when a player picks them:
//
//   44 LicenseScreen        45 Exit after 5 s of legal text: taken at once; the logo videos
//                           (CMoviePlayerBehaviour, below) are skipped
//   46 LoadPersistentPostBootUp  -> 52 ExitTitleScreen when the persistent data has loaded (51
//                           SkipToLevel, the developers' way to InGame, when the game state is
//                           already 5, a level)
//   59 TitleScreen          65 ExitAccept (START) -> 70 MainMenuScreen. Here START is really
//                           pressed (Buttons): the controller that presses it becomes the player,
//                           and without one LOAD GAME goes to 114 NoProfile
//   70 MainMenuScreen       level:<name>: StartLevel + 51 SkipToLevel (the tree takes it from here
//                           too), after 2 s in the menu: the menus need that time to make the
//                           controller player 1 (leaving at once, the first START in the level
//                           joins a player 2 and the pause never opens)
//                           load:<slot>: 89 ExitLoadGame (Xbox) -> 111 AccessMemoryCard ->
//  154 GameSlotScreen       180 + 20 * (slot - 1) ExitLoad > ExitSlotNotEmpty: LoadGame(slot - 1)
//  308 LoadComplete         309 ExitToHUB -> 575 HUBScreen (the episode menu)
//  575 HUBScreen            604 ExitContinueStory (StartNextLevel): Y is really pressed here
//                           (Buttons), about 1 s in, for the same reason as above. For
//                           load:N:<level> the index StartNextLevel sets is swapped (sub_821F9EC8).
//  630 InGame               the level has loaded; then 633 LevelIntroCinematic or 638 GameRunning
//
// A level is started with Frontend:StartLevel(index) (sub_820F5D58: the managers object's level
// index at +24, game state 5), the same as StartLevelByName in the snippets; the level list is the
// managers object's (0x824A7A24: count at +316, level objects at +156, name at level+32).
//
// The logo videos: CMoviePlayerBehaviour::PlayMovie(index) (sub_8214ABD0) plays d:\movies\<name>
// from the table at 0x8245F224 (0 dolby, 1 radical, 2 sierra, 3 shadow, 4 attract) and its
// callback (sub_8214B210) plays the next one when one ends. Skipping asks for the Sierra logo
// instead, which this game's discs don't have: the player fails to open it and the callback goes
// on at once, the path the game always takes for that one.

#include "direct_boot.h"
#include "pc_frontend.h"
#include "settings.h"
#include "skip_cinematics.h"
#include "../diagnostics.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../platform/platform.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_8214ABD0);  // CMoviePlayerBehaviour::PlayMovie(index)
extern "C" PPC_FUNC(__imp__sub_821F9EC8);  // managers: set the level index (StartLevel)
extern "C" PPC_FUNC(__imp__sub_820F5D58);  // Frontend:StartLevel(index)

namespace
{
    // Fight tree numbers of this version's Frontend.bfig (checked with settings::QuitExit, which
    // pc_frontend.cpp finds by name: 86 when the tree is the one these were read from).
    constexpr int32_t KNOWN_QUIT_EXIT = 86;
    constexpr uint32_t NODE_LICENSE = 44, NODE_LOAD_PERSISTENT = 46, NODE_TITLE = 59, NODE_MAIN_MENU = 70;
    constexpr uint32_t NODE_NO_SAVES = 151, NODE_SLOTS = 154, NODE_INGAME_SLOTS = 421, NODE_LOAD_COMPLETE = 308, NODE_LOAD_FAILED = 313;
    constexpr uint32_t NODE_HUB = 575, NODE_IN_GAME = 630, NODE_LEVEL_INTRO = 633, NODE_GAME_RUNNING = 638;
    constexpr uint32_t NODE_CINEMATICS = 655, NODE_CINEMATICS_END = 1051, NODE_PAUSE = 1081, NODE_GAME_OVER = 1192;
    constexpr int32_t EXIT_LICENSE = 45, EXIT_SKIP_TO_LEVEL = 51, EXIT_LOAD_GAME = 89;
    constexpr int32_t EXIT_SLOT1_LOAD = 180, EXIT_SLOT_STEP = 20, EXIT_TO_HUB = 309;

    constexpr uint32_t MANAGERS = 0x824A7A24;  // sub_821FB4F0
    constexpr uint32_t LEVEL_INDEX = 24, LEVELS = 156, LEVEL_COUNT = 316, LEVEL_NAME = 32;
    constexpr uint32_t MAX_LEVELS = (LEVEL_COUNT - LEVELS) / 4;

    constexpr int32_t MOVIE_SIERRA = 2, MOVIE_ATTRACT = 4;

    // A screen the boot picks something on is left alone for this many evaluations first (one per
    // frame): its page and input come up the way they do for a player.
    constexpr int SETTLE_EVALUATIONS = 3;
    // Time in the menus (game ticks, 30 per second) before a level starts from them: the game makes
    // the controller that pressed START player 1 meanwhile. Leaving at once (3 frames) starts the
    // level with no player 1: the first START there joins player 2 and the pause never opens.
    constexpr int64_t MENU_SETTLE_TICKS = 60;
    // START on the title screen: held this long, every this often (a press during the screen's
    // fade-in is ignored).
    constexpr uint64_t START_HELD_MS = 150, START_PERIOD_MS = 400;
    // 638 GameRunning for this many game ticks (30 per second, whatever the frame rate) = the player
    // has control: it also shows for a moment before a level's opening cutscene (0.5 s in L1_E3).
    constexpr int64_t GAMEPLAY_TICKS = 45;

    enum class Mode { None, Menu, Level, Load };
    enum class After { Continue, Hub, Level };

    struct Config
    {
        bool skipIntros = false;
        bool skipCinematics = false;
        Mode mode = Mode::None;
        std::string spec;
        std::string level;    // Level / Load + After::Level
        int slot = 1;         // Load: 1..5
        After after = After::Continue;
    };

    Config g_config;
    bool g_configured = false;

    std::string Upper(std::string text)
    {
        for (char& c : text)
            c = char(toupper(uint8_t(c)));
        return text;
    }

    bool LooksLikeLevel(const std::string& text)
    {
        return text.size() >= 5 && (text[0] == 'L' || text[0] == 'l') && isdigit(uint8_t(text[1])) && text[2] == '_';
    }

    bool ParseSpec(const std::string& spec, Config& config)
    {
        std::string lower = spec;
        for (char& c : lower)
            c = char(tolower(uint8_t(c)));
        if (lower.empty() || lower == "0" || lower == "off" || lower == "none")
            return true;
        config.spec = spec;
        config.skipIntros = true;
        if (lower == "menu")
        {
            config.mode = Mode::Menu;
            return true;
        }
        if (lower.rfind("level:", 0) == 0 || LooksLikeLevel(spec))
        {
            config.mode = Mode::Level;
            config.level = Upper(lower.rfind("level:", 0) == 0 ? spec.substr(6) : spec);
            return !config.level.empty();
        }
        if (lower.rfind("load", 0) == 0)
        {
            config.mode = Mode::Load;
            std::string rest = spec.size() > 4 && spec[4] == ':' ? spec.substr(5) : std::string();
            size_t colon = rest.find(':');
            std::string slot = rest.substr(0, colon), what = colon == std::string::npos ? std::string() : rest.substr(colon + 1);
            config.slot = slot.empty() ? 1 : atoi(slot.c_str());
            if (config.slot < 1 || config.slot > 5)
                return false;
            std::string w = what;
            for (char& c : w)
                c = char(tolower(uint8_t(c)));
            if (w.empty() || w == "continue")
                config.after = After::Continue;
            else if (w == "hub")
                config.after = After::Hub;
            else if (LooksLikeLevel(what))
                config.after = After::Level, config.level = Upper(what);
            else
                return false;
            return true;
        }
        return false;
    }

    bool Enabled(const char* text)
    {
        return text && *text && strcmp(text, "0") != 0;
    }

    void Configure(const char* skipArgument, const char* bootArgument, const char* cinematicsArgument)
    {
        Config config;
        config.skipIntros = Enabled(skipArgument ? skipArgument : getenv("CRASH_SKIP_INTROS"));
        if (settings::Restarted())
            config.skipIntros = true;  // back from the settings screen: straight to the menu
        const char* spec = bootArgument ? bootArgument : getenv("CRASH_BOOT");
        bool skipAsked = config.skipIntros;
        if (spec && !ParseSpec(spec, config))
        {
            Log("[test] boot \"%s\" not understood (use menu, level:L1_E2 or load:<slot 1-5>[:continue|hub|L1_E2]); booting normally", spec);
            config = Config{};
            config.skipIntros = skipAsked;
        }
        if (config.skipIntros && config.mode == Mode::None)
            config.mode = Mode::Menu;
        config.skipCinematics = Enabled(cinematicsArgument ? cinematicsArgument : getenv("CRASH_SKIP_CINEMATICS"));
        g_config = config;
        g_configured = true;
        if (config.mode == Mode::Menu)
            Log("[test] boot: skipping the intros to the main menu%s", settings::Restarted() ? " (restarted from the settings screen)" : "");
        else if (config.mode == Mode::Level)
            Log("[test] boot: straight into level %s (no save)", config.level.c_str());
        else if (config.mode == Mode::Load)
            Log("[test] boot: LOAD GAME slot %d, then %s", config.slot,
                config.after == After::Continue ? "CONTINUE STORY" : config.after == After::Hub ? "the episode menu" : config.level.c_str());
        if (config.skipCinematics)
            Log("[test] boot: skipping the in-game cutscenes");
    }

    const Config& TheConfig()
    {
        if (!g_configured)
            Configure(nullptr, nullptr, nullptr);
        return g_config;
    }

    // Events --------------------------------------------------------------------------------

    struct EventTimes
    {
        std::mutex mutex;
        std::vector<std::pair<std::string, int64_t>> seen;  // name, input clock ms (in order)
    };
    EventTimes g_events;

    // Boot progress (the frontend's thread only).
    struct Progress
    {
        uint32_t node = UINT32_MAX;
        int evaluations = 0;       // of `node`, in a row
        int64_t since = 0;         // game tick `node` was entered
        bool introDone = false;     // reached the main menu (or a level) once: no more skipping
        bool licenseLeft = false;
        bool levelStarted = false;  // Level: StartLevel called
        bool loadAsked = false, slotChosen = false, hubLeft = false;
        bool failed = false;
        bool loading = false;       // in 630 InGame (loading)
        bool gameplay = false;      // gameplay_ready sent for this stretch of 638
        bool numbersChecked = false, numbersOk = false;
        int32_t levelOverride = -1; // load:N:<level>: the index StartNextLevel's level becomes
    };
    Progress g_progress;

    // The level list, once the game has it.
    std::vector<std::string> Levels()
    {
        std::vector<std::string> names;
        uint32_t count = LoadBE32(MANAGERS + LEVEL_COUNT);
        if (count > MAX_LEVELS)
            return names;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t level = LoadBE32(MANAGERS + LEVELS + 4 * i);
            uint32_t name = level ? LoadBE32(level + LEVEL_NAME) : 0;
            names.push_back(name ? std::string(reinterpret_cast<const char*>(g_memory.Translate(name))) : std::string("?"));
        }
        return names;
    }

    int32_t LevelIndex(const std::string& name)
    {
        std::vector<std::string> levels = Levels();
        for (size_t i = 0; i < levels.size(); i++)
        {
            if (_stricmp(levels[i].c_str(), name.c_str()) == 0)
                return int32_t(i);
        }
        return -1;
    }

    void LogLevels()
    {
        static bool logged = false;
        std::vector<std::string> levels = Levels();
        if (logged || levels.empty())
            return;
        logged = true;
        std::string list;
        for (size_t i = 0; i < levels.size(); i++)
            list += (i ? ", " : "") + std::to_string(i) + " " + levels[i];
        Log("[test] levels (%zu): %s", levels.size(), list.c_str());
    }

    bool NumbersOk()
    {
        Progress& p = g_progress;
        if (!p.numbersChecked)
        {
            p.numbersChecked = true;
            p.numbersOk = settings::QuitExit() == KNOWN_QUIT_EXIT;
            if (!p.numbersOk)
                Log("[test] boot: this Frontend.bfig is not the one the boot knows (EXIT GAME exit %d, expected %d); booting normally",
                    settings::QuitExit(), KNOWN_QUIT_EXIT);
        }
        return p.numbersOk;
    }

    void Fail(const char* why)
    {
        if (g_progress.failed)
            return;
        g_progress.failed = true;
        boot::Event("boot_failed", why);
    }

    // Calls Frontend:StartLevel(index) as a snippet would.
    void StartLevel(PPCContext& ctx, uint8_t* base, uint32_t tree, int32_t index)
    {
        PPCContext call = ctx;
        call.r3.u64 = tree;
        call.r4.s64 = index;
        __imp__sub_820F5D58(call, base);
    }
}

namespace boot
{
    void HandleArguments(int argc, char** argv)
    {
        const char* skip = nullptr;
        const char* spec = nullptr;
        const char* cinematics = nullptr;
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "--skip-intros") == 0)
                skip = "1";
            else if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc)
                spec = argv[++i];
            else if (strcmp(argv[i], "--skip-cinematics") == 0)
                cinematics = "1";
        }
        Configure(skip, spec, cinematics);
    }

    bool SkipIntros()
    {
        return TheConfig().skipIntros && !g_progress.introDone;
    }

    bool SkipCinematics()
    {
        return TheConfig().skipCinematics;
    }

    uint16_t Buttons()
    {
        const Config& config = TheConfig();
        const Progress& p = g_progress;
        if (config.mode == Mode::None || p.failed || p.evaluations < SETTLE_EVALUATIONS)
            return 0;
        uint16_t button = 0;
        if (p.node == NODE_TITLE && !p.introDone)
            button = XINPUT_GAMEPAD_START;
        // CONTINUE STORY with a real Y: the controller that starts the level from the episode menu is
        // the one the pause menu listens to (a forced exit leaves the pause dead in that level). For
        // another level than the next one, the level index is swapped where it is set (sub_821F9EC8).
        else if (p.node == NODE_HUB && config.mode == Mode::Load && config.after != After::Hub && p.slotChosen && !p.hubLeft)
            button = XINPUT_GAMEPAD_Y;
        if (!button)
            return 0;
        return plat::TickCountMs() % START_PERIOD_MS < START_HELD_MS ? button : 0;
    }

    void Event(const char* name, const char* detail)
    {
        int64_t ms = input::ClockMs();
        {
            std::lock_guard lock(g_events.mutex);
            g_events.seen.emplace_back(name, ms);
        }
        Log("[test] event %s (frame %llu, input clock %.2f s, tick %lld)%s%s", name, (unsigned long long)input::Frame(), double(ms) / 1000.0,
            (long long)input::GameTick(), detail ? ": " : "", detail ? detail : "");
        input::NoteEvent(name);
    }

    bool ParseAnchored(const char*& text, double unitMs, Anchored& out)
    {
        out = Anchored{};
        const char* p = text;
        while (*p == ' ')
            p++;
        if (*p == '@' || isalpha(uint8_t(*p)))
        {
            if (*p == '@')
                p++;
            const char* start = p;
            while (isalnum(uint8_t(*p)) || *p == '_')
                p++;
            out.event.assign(start, p);
            if (out.event.empty())
                return false;
            if (*p == '#')
            {
                char* end;
                out.occurrence = std::max(1, int(strtol(p + 1, &end, 10)));
                p = end;
            }
            if (*p != '+' && *p != '-')
            {
                text = p;
                return true;
            }
        }
        char* end;
        double value = strtod(p, &end);
        if (end == p)
            return false;
        out.offsetMs = int64_t(value * unitMs);
        text = end;
        return true;
    }

    int64_t Resolve(const Anchored& time)
    {
        if (time.event.empty())
            return time.offsetMs;
        std::lock_guard lock(g_events.mutex);
        int seen = 0;
        for (const auto& [name, ms] : g_events.seen)
        {
            if (name == time.event && ++seen == time.occurrence)
                return std::max<int64_t>(ms + time.offsetMs, 0);
        }
        return -1;
    }

    void NoteFrontendNode(uint32_t node)
    {
        cutscenes::NoteFrontendNode(node);
        Progress& p = g_progress;
        if (node == p.node)
        {
            p.evaluations++;
            if (node == NODE_GAME_RUNNING && !p.gameplay && input::GameTick() - p.since >= GAMEPLAY_TICKS)
            {
                p.gameplay = true;
                Event("gameplay_ready");
            }
            return;
        }
        uint32_t previous = p.node;
        if (previous == NODE_HUB && p.slotChosen)
            p.hubLeft = true;  // the boot's Y (or a forced exit) took it out of the episode menu
        p.node = node;
        p.evaluations = 1;
        p.since = input::GameTick();
        if (node != NODE_GAME_RUNNING)
            p.gameplay = false;

        if (node >= NODE_LOAD_PERSISTENT)
            LogLevels();
        if (node == NODE_MAIN_MENU)
        {
            p.introDone = true;
            Event("main_menu");
        }
        else if (node == NODE_TITLE)
            Event("title");
        else if (node == NODE_SLOTS || node == NODE_INGAME_SLOTS)
            Event("save_slots");
        else if (node == NODE_LOAD_COMPLETE)
            Event("load_complete");
        else if (node == NODE_HUB)
            Event("hub");
        else if (node == NODE_IN_GAME)
        {
            // Shown once the level has loaded (the frontend doesn't run during the load).
            p.introDone = true;
            p.loading = true;
        }
        else if (node == NODE_PAUSE)
            Event("pause");
        else if (node == NODE_GAME_OVER)
            Event("game_over");
        else if (node == NODE_LEVEL_INTRO || (node >= NODE_CINEMATICS && node <= NODE_CINEMATICS_END))
        {
            if (previous < NODE_CINEMATICS || previous > NODE_CINEMATICS_END)
                Event("cinematic", std::to_string(node).c_str());
        }
        if (p.loading && node != NODE_IN_GAME)
        {
            p.loading = false;
            Event("level_ready");
        }
        if (TheConfig().mode == Mode::Load && !p.failed && (node == NODE_LOAD_FAILED || node == NODE_NO_SAVES))
            Fail(node == NODE_NO_SAVES ? "no saves to load" : "the save did not load");
    }

    int32_t OverrideExit(PPCContext& ctx, uint8_t* base, uint32_t tree, uint32_t node, int32_t exit)
    {
        const Config& config = TheConfig();
        Progress& p = g_progress;
        if (config.mode == Mode::None || p.failed || node != p.node || !NumbersOk())
            return exit;
        auto take = [&](int32_t chosen, const char* what) {
            Log("[test] boot: node %u -> exit %d (%s)%s", node, chosen, what, exit >= 0 ? (" instead of " + std::to_string(exit)).c_str() : "");
            return chosen;
        };

        if (exit >= 0 || p.evaluations < SETTLE_EVALUATIONS)
            return exit;

        // A level: from the main menu, once the menus have had the time to make the controller that
        // pressed START player 1, the developers' SkipToLevel exit (node 46's; the tree takes it from
        // here too) after the game's own StartLevel.
        if (config.mode == Mode::Level && node == NODE_MAIN_MENU && !p.levelStarted && input::GameTick() - p.since >= MENU_SETTLE_TICKS)
        {
            p.levelStarted = true;
            int32_t index = LevelIndex(config.level);
            if (index < 0)
            {
                std::string known;
                for (const std::string& level : Levels())
                    known += " " + level;
                Fail(("no level " + config.level + " (levels:" + known + ")").c_str());
                return exit;
            }
            StartLevel(ctx, base, tree, index);
            return take(EXIT_SKIP_TO_LEVEL, ("SkipToLevel, StartLevel " + std::to_string(index) + " " + config.level).c_str());
        }

        if (node == NODE_LICENSE && !p.introDone && !p.licenseLeft)
        {
            p.licenseLeft = true;
            return take(EXIT_LICENSE, "legal text: no 5 s wait");
        }
        if (config.mode != Mode::Load)
            return exit;
        if (node == NODE_MAIN_MENU && !p.loadAsked)
        {
            p.loadAsked = true;
            return take(EXIT_LOAD_GAME, "main menu: LOAD GAME");
        }
        if (node == NODE_SLOTS && p.loadAsked && !p.slotChosen)
        {
            p.slotChosen = true;
            return take(EXIT_SLOT1_LOAD + EXIT_SLOT_STEP * (config.slot - 1), ("save slot " + std::to_string(config.slot)).c_str());
        }
        if (node == NODE_LOAD_COMPLETE && p.slotChosen)
            return take(EXIT_TO_HUB, "load complete: CONTINUE");
        if (node == NODE_HUB && p.slotChosen && !p.hubLeft)
        {
            if (config.after == After::Hub)
            {
                p.hubLeft = true;
                Event("boot_done", "episode menu");
                return exit;
            }
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                if (config.after == After::Level)
                {
                    p.levelOverride = LevelIndex(config.level);
                    if (p.levelOverride < 0)
                    {
                        Fail(("no level " + config.level).c_str());
                        return exit;
                    }
                }
                Log("[test] boot: node %u: pressing Y (CONTINUE STORY)%s", node,
                    config.after == After::Level ? (", level " + std::to_string(p.levelOverride) + " " + config.level + " in place of the next one").c_str() : "");
            }
            return exit;  // Buttons() presses Y
        }
        return exit;
    }
}

// CMoviePlayerBehaviour::PlayMovie(index): the logo videos ask for the missing Sierra one.
PPC_FUNC(sub_8214ABD0)
{
    int32_t movie = ctx.r4.s32;
    if (boot::SkipIntros() && movie != MOVIE_ATTRACT && movie != MOVIE_SIERRA)
    {
        Log("[test] skip intros: logo video %d skipped", movie);
        ctx.r4.s64 = MOVIE_SIERRA;
    }
    cutscenes::NoteMovie(ctx.r4.s32);  // the skip prompt
    __imp__sub_8214ABD0(ctx, base);
}

// The managers' level index (every way of starting a level): the level_start event, and the level
// load:N:<level> asked for in place of the one CONTINUE STORY picked.
PPC_FUNC(sub_821F9EC8)
{
    if (g_progress.levelOverride >= 0)
    {
        Log("[test] boot: level index %d -> %d", ctx.r4.s32, g_progress.levelOverride);
        ctx.r4.s64 = g_progress.levelOverride;
        g_progress.levelOverride = -1;
    }
    int32_t index = ctx.r4.s32;
    __imp__sub_821F9EC8(ctx, base);
    std::vector<std::string> levels = Levels();
    std::string detail = (index >= 0 && size_t(index) < levels.size() ? levels[index] : std::string("?")) + " (index " + std::to_string(index) + ")";
    boot::Event("level_start", detail.c_str());
}
