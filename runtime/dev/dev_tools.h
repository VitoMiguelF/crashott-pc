#pragma once

// Developer leftovers of the retail game, exposed for tests and modding (dev_tools.cpp;
// docs/dev_features.md, README "Recursos DEV").
//
//   CRASH_DEV_LUA="<lua>"          Lua run inside the game's own script state (the game's
//                                  luaL_loadbuffer + lua_pcall), on the game thread, between two
//                                  script updates. Errors and print() go to the log as "[lua] ...".
//   CRASH_DEV_LUA_AT=<time>        when: "<event>[#n][+seconds]" (direct_boot.h events) or a plain
//                                  number of seconds on the input clock. Default: gameplay_ready.
//   CRASH_DEV_LUA_FILE=<path>      a file of blocks; a line "@<time>" starts a block run at that time
//                                  (text before the first one runs at CRASH_DEV_LUA_AT).
//   CRASH_DEV_INVINCIBLE=1         the players' hitpoints never go down (dev_cheats.cpp).
//
// Shortcuts, each a dev.* call (dev_library.lua.inc) run at CRASH_DEV_LUA_AT before CRASH_DEV_LUA:
//   CRASH_DEV_TELEPORT=x,y,z       player 1 to that position (dev.pos() logs the current one)
//   CRASH_DEV_MOJO=<n>             +n mojo (the HUD counter follows)
//   CRASH_DEV_UNLOCK_ALL=1         every skin (Crash_Hulk too), level, play zone, mojo room, move...
//   CRASH_DEV_SHADOW=1             the game's Shadow Crash cheat (SettingsDatabase m_CheatShadowCrash)
//   CRASH_DEV_GAMELINK=1           loads the game's GameLinkUtil (Radical's remote debug helpers)
//
// Before the first block the runtime defines devprint (the base library's print, to the log),
// _TRACEBACK (without it every Lua runtime error crashed or became "error in error handling")
// and the dev.* library.
//
// Every block logs "[dev] lua block N at <time>: ok" (or the error) and the event "dev_lua_<N>".

#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace dev
{
    // Called once per frame by the hook on the game's script update (game thread).
    void RunPendingLua(PPCContext& ctx, uint8_t* base);
    // Runs `code` now in the game's Lua state (game thread only). False on a compile/run error
    // (its message in `error`, also logged).
    bool RunLua(PPCContext& ctx, uint8_t* base, const char* code, const char* chunkName, std::string* error = nullptr);

    // dev_cheats.cpp: invincibility on / off (CRASH_DEV_INVINCIBLE sets it at startup; the console).
    void SetInvincible(bool on);
    bool Invincible();
    // The game's Big Head cheat (SettingsDatabase m_CheatBigHeadCrash + the head scale of the
    // players, as its cheat code does). Returns how many players got it.
    int SetBigHead(PPCContext& ctx, uint8_t* base, bool on);
    // Mojo (EStatInt_MojoCount) + `amount` (negative takes it away), kept in 0..MOJO_MAX. Game thread.
    constexpr int64_t MOJO_MAX = 2147483647;
    struct MojoResult
    {
        bool ok = false;       // the stats manager exists (a game is loaded)
        bool clamped = false;  // `amount` went past 0 or MOJO_MAX
        int64_t added = 0;
        uint32_t total = 0;
    };
    MojoResult GiveMojo(PPCContext& ctx, uint8_t* base, int64_t amount);
    // One more life (EStatInt_LifeCount, up to 99); the lives now, -1 without a game. Game thread.
    int AddLife(PPCContext& ctx, uint8_t* base);
    int Lives(PPCContext& ctx, uint8_t* base);
    // Any hit stuns a titan (its stamina meter empties); the enemies' AI stands still.
    void SetOneHitTitans(bool on);
    bool OneHitTitans();
    void SetAiDisabled(bool on);
    bool AiDisabled();

    // The game's level list (the managers object), e.g. "L1_E1"; empty before it is loaded.
    std::vector<std::string> Levels();

    // dev_skins.cpp: Crash's skins (ESkin numbers).
    namespace skins
    {
        constexpr int CRASH = 21, HULK = 22;
        int ByName(const std::string& name);  // "ratcicle", "hulk", "ESkin_Koala", "9"...; -1 unknown
        const char* Label(int skin);
        bool Wearable(int skin);               // not 18-20 (no name: crash) or Cortex 1 (no model)
        std::vector<std::string> Names();      // the wearable ones, for completion
        std::string List();                    // every name, for help
        int Current(PPCContext& ctx, uint8_t* base);  // the skin worn now, -1 if unknown
        int Pending();                          // waiting for the next level load, -1 none
        bool InLevel();                         // in a level (loaded, with a player 1), not the HUB / menus
        // [dev] hulk_skin: Crash_Hulk counts as unlocked (skin menu). True if it changed.
        bool UnlockHulk(bool on);
        // Wears `skin`: in the frontend at once (the game's SelectSkin), in a level at the next
        // level load (`now`: restarts the level). Returns the message for the console.
        // `frontend`: the frontend's way even if a level was loaded (the skin screen: the menus' scene loads as one).
        std::string Select(PPCContext& ctx, uint8_t* base, int skin, bool now, bool frontend = false);
    }
}
