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

struct PPCContext;

namespace dev
{
    // Called once per frame by the hook on the game's script update (game thread).
    void RunPendingLua(PPCContext& ctx, uint8_t* base);
    // Runs `code` now in the game's Lua state (game thread only). False on a compile/run error.
    bool RunLua(PPCContext& ctx, uint8_t* base, const char* code, const char* chunkName);
}
