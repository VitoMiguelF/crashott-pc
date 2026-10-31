// Lua inside the game's own script state, for tests and modding (dev_tools.h; docs/dev_features.md).
//
// The retail game still carries the whole Lua 5.1 compiler (luaL_loadbuffer parses source text, the
// base library has loadstring/dofile), so source code can be run in its state at any time:
//
//   script::ScriptManager   *(0x824FBFE8); its lua_State at +1056 (sub_8209F588, sub_822DCDE8)
//   luaL_loadbuffer         sub_823C9530(L, buffer, size, chunk name) -> 0 = ok (base loadstring)
//   lua_pcall               sub_822995E8(L, nargs, nresults, errfunc)
//   lua_gettop / settop     sub_82298360(L) / sub_82298450(L, index)
//   lua_tostring            sub_822989A0(L, index)
//   CScriptManager::vf10    sub_82230DE0(this, f1 = dt): the per-frame script update (coroutines,
//                           then a GC step), run by the game loop outside any Lua call: the point
//                           where the blocks run.
//   Lua print (base lib)    sub_823CCF40 writes with fputs (sub_823D3410) to the guest CRT's stdout
//                           (__iob_func() = 0x82480F50, stdout = +32, stderr = +64), which went
//                           nowhere: the fputs hook sends stdout/stderr lines to the log. The
//                           scripts replace the global print with a no-op in the retail game, so
//                           the base print is registered again as the global "devprint"
//                           (lua_pushcclosure sub_82298EA0, lua_setfield sub_822992D0, as the
//                           CScriptManager constructor sub_822319D8 registers its own globals).
//                           (objectlog, the scripts' debug log, can't be restored: its retail body
//                           is the "return 0" shared by 540 functions, sub_82303190.)
//   _TRACEBACK              luaG_errormsg (sub_822A2C28) is Radical's: with no errfunc it calls the
//                           global _TRACEBACK (set by the debug library, absent in the retail
//                           game); without it every runtime error becomes LUA_ERRERR ("error in
//                           error handling") or crashes. The first block defines one that logs
//                           the message ("[lua] erro: ...") and returns it, so errors are
//                           catchable with pcall and show up in the log.
//
// What the scripts can call is the game's own API (managers, actors, stats, SettingsDatabase...):
// docs/dev_features.md lists it, dumped from the running game with dev/lua/dump_globals.lua.

#include "dev_tools.h"
#include "dev_console.h"
#include "cheat_menu.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../settings/direct_boot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PPC_EXTERN_FUNC(sub_823C9530);  // luaL_loadbuffer
PPC_EXTERN_FUNC(sub_822995E8);  // lua_pcall
PPC_EXTERN_FUNC(sub_82298360);  // lua_gettop
PPC_EXTERN_FUNC(sub_82298450);  // lua_settop
PPC_EXTERN_FUNC(sub_822989A0);  // lua_tostring
PPC_EXTERN_FUNC(sub_82298EA0);  // lua_pushcclosure
PPC_EXTERN_FUNC(sub_822992D0);  // lua_setfield
extern "C" PPC_FUNC(__imp__sub_82230DE0);  // CScriptManager::vf10 (per-frame update)
extern "C" PPC_FUNC(__imp__sub_823D3410);  // fputs (guest CRT)

namespace
{
    constexpr uint32_t SCRIPT_MANAGER_PTR = 0x824FBFE8;
    constexpr uint32_t SCRIPT_MANAGER_LUA_STATE = 1056;
    constexpr uint32_t GUEST_STDOUT = 0x82480F70, GUEST_STDERR = 0x82480F90;
    constexpr uint32_t LUA_BASE_PRINT = 0x823CCF40;
    constexpr int64_t LUA_GLOBALSINDEX = -10001;  // as the game uses it (sub_822319D8, sub_822DCF40)

    struct Block
    {
        boot::Anchored at;
        std::string atText;
        std::string code;
        bool done = false;
    };

    struct State
    {
        std::vector<Block> blocks;
        uint32_t buffer = 0, bufferSize = 0, chunkName = 0, printName = 0;
        uint32_t printState = 0;  // the lua_State devprint was registered in
        std::string stdoutLine;
        std::mutex stdoutMutex;
    };

    State& TheState()
    {
        static State state;
        return state;
    }

    bool ParseTime(const std::string& text, Block& block)
    {
        const char* p = text.c_str();
        block.atText = text;
        return boot::ParseAnchored(p, 1000.0, block.at);
    }

    std::vector<Block> LoadBlocks()
    {
        std::vector<Block> blocks;
        const char* atEnv = getenv("CRASH_DEV_LUA_AT");
        std::string defaultAt = atEnv && *atEnv ? atEnv : "gameplay_ready";
        auto add = [&](const std::string& at, const std::string& code) {
            if (code.find_first_not_of(" \t\r\n") == std::string::npos)
                return;
            Block block;
            if (!ParseTime(at, block))
            {
                Log("[dev] lua: bad time \"%s\" (use <event>[#n][+seconds] or seconds)", at.c_str());
                return;
            }
            block.code = code;
            blocks.push_back(std::move(block));
        };
        // Shortcuts: each one is a dev.* call at CRASH_DEV_LUA_AT (dev_library.lua.inc).
        auto shortcut = [&](const char* name, const std::string& before, const std::string& after) {
            const char* value = getenv(name);
            if (value && *value && strcmp(value, "0") != 0)
                add(defaultAt, before + value + after);
        };
        shortcut("CRASH_DEV_GAMELINK", "if ", " then dev.gamelink() devprint('dev.gamelink loaded') end");
        shortcut("CRASH_DEV_TELEPORT", "dev.teleport(", ")");
        shortcut("CRASH_DEV_MOJO", "dev.give_mojo(", ")");
        shortcut("CRASH_DEV_UNLOCK_ALL", "if ", " then dev.unlock_all() end");
        shortcut("CRASH_DEV_SHADOW", "dev.setting('m_CheatShadowCrash', ", " ~= 0)");
        if (const char* code = getenv("CRASH_DEV_LUA"); code && *code)
            add(defaultAt, code);
        if (const char* path = getenv("CRASH_DEV_LUA_FILE"); path && *path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                Log("[dev] lua: can't read CRASH_DEV_LUA_FILE %s", path);
            std::string line, at = defaultAt, code;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && line[0] == '@')
                {
                    add(at, code);
                    at = line.substr(1);
                    code.clear();
                    continue;
                }
                code += line;
                code += '\n';
            }
            add(at, code);
        }
        for (size_t i = 0; i < blocks.size(); i++)
            Log("[dev] lua block %zu at %s: %zu bytes", i + 1, blocks[i].atText.c_str(), blocks[i].code.size());
        return blocks;
    }

    uint32_t LuaState()
    {
        uint32_t manager = LoadBE32(SCRIPT_MANAGER_PTR);
        return manager ? LoadBE32(manager + SCRIPT_MANAGER_LUA_STATE) : 0;
    }

    std::string GuestCString(uint32_t address)
    {
        if (!address)
            return {};
        const char* text = reinterpret_cast<const char*>(g_memory.Translate(address));
        return std::string(text, strnlen(text, 4096));
    }

    void WriteStdout(const char* text, bool error)
    {
        State& state = TheState();
        std::lock_guard lock(state.stdoutMutex);
        for (const char* p = text; *p; p++)
        {
            if (*p == '\n')
            {
                Log("[lua]%s %s", error ? " (stderr)" : "", state.stdoutLine.c_str());
                const std::string& line = state.stdoutLine;
                dev::console::Print(line, error || line.rfind("erro: ", 0) == 0 ? dev::console::Tone::Error : dev::console::Tone::Lua);
                state.stdoutLine.clear();
            }
            else if (state.stdoutLine.size() < 4096)
                state.stdoutLine += *p;
        }
    }
}

namespace dev
{
    bool RunLua(PPCContext& ctx, uint8_t* base, const char* code, const char* chunkName, std::string* error)
    {
        State& state = TheState();
        uint32_t L = LuaState();
        if (!L)
        {
            Log("[dev] lua: no script state yet");
            return false;
        }
        size_t size = strlen(code);
        if (size + 1 > state.bufferSize)
        {
            // Bump allocator (never freed): grow by doubling, so a few large blocks cost little.
            state.bufferSize = uint32_t(std::max<size_t>(size + 1, std::max<uint32_t>(64 * 1024, state.bufferSize * 2)));
            state.buffer = GuestAlloc(state.bufferSize);
        }
        if (!state.chunkName)
            state.chunkName = GuestAlloc(64);
        if (!state.buffer || !state.chunkName)
        {
            Log("[dev] lua: out of runtime guest memory");
            return false;
        }
        if (state.printState != L)
        {
            // devprint = the base library's print (stdout -> fputs hook -> log).
            if (!state.printName)
                state.printName = GuestAlloc(16);
            memcpy(g_memory.Translate(state.printName), "devprint", 9);
            PPCContext call = ctx;
            call.r3.u64 = L;
            call.r4.u64 = LUA_BASE_PRINT;
            call.r5.u64 = 0;
            sub_82298EA0(call, base);
            call = ctx;
            call.r3.u64 = L;
            call.r4.u64 = uint64_t(LUA_GLOBALSINDEX);
            call.r5.u64 = state.printName;
            sub_822992D0(call, base);
            state.printState = L;
            static const char* traceback =
                "if type(_TRACEBACK) ~= 'function' then _TRACEBACK = function(m) devprint('erro: ' .. tostring(m)) return m end end";
            RunLua(ctx, base, traceback, "dev_traceback");
            static const char* library =
#include "dev_library.lua.inc"
                ;
            RunLua(ctx, base, library, "dev_library");
        }
        memcpy(g_memory.Translate(state.buffer), code, size + 1);
        std::string name = std::string("=") + (chunkName ? chunkName : "dev");
        name.resize(std::min<size_t>(name.size(), 63));
        memcpy(g_memory.Translate(state.chunkName), name.c_str(), name.size() + 1);

        PPCContext call = ctx;
        call.r3.u64 = L;
        sub_82298360(call, base);
        uint32_t top = call.r3.u32;

        call = ctx;
        call.r3.u64 = L;
        call.r4.u64 = state.buffer;
        call.r5.u64 = size;
        call.r6.u64 = state.chunkName;
        sub_823C9530(call, base);
        int status = int(call.r3.s32);
        if (status == 0)
        {
            call = ctx;
            call.r3.u64 = L;
            call.r4.u64 = 0;
            call.r5.u64 = 0;
            call.r6.u64 = 0;
            sub_822995E8(call, base);
            status = int(call.r3.s32);
        }
        if (status != 0)
        {
            call = ctx;
            call.r3.u64 = L;
            call.r4.u64 = uint64_t(-1);
            call.r5.u64 = 0;
            sub_822989A0(call, base);
            std::string message = GuestCString(call.r3.u32);
            Log("[dev] lua error (%d) in %s: %s", status, name.c_str() + 1, message.c_str());
            if (error)
                *error = message;
        }
        call = ctx;
        call.r3.u64 = L;
        call.r4.u64 = top;
        sub_82298450(call, base);
        return status == 0;
    }

    std::vector<std::string> Levels()
    {
        // The managers object (0x824A7A24, sub_821FB4F0): level objects at +156, count at +316,
        // a level's name at +32 (as settings/direct_boot.cpp reads them).
        constexpr uint32_t MANAGERS = 0x824A7A24, LEVELS = 156, LEVEL_COUNT = 316, LEVEL_NAME = 32;
        std::vector<std::string> names;
        uint32_t count = LoadBE32(MANAGERS + LEVEL_COUNT);
        if (count > (LEVEL_COUNT - LEVELS) / 4)
            return names;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t level = LoadBE32(MANAGERS + LEVELS + 4 * i);
            uint32_t name = level ? LoadBE32(level + LEVEL_NAME) : 0;
            names.push_back(name ? GuestCString(name) : std::string("?"));
        }
        return names;
    }

    void RunPendingLua(PPCContext& ctx, uint8_t* base)
    {
        State& state = TheState();
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            state.blocks = LoadBlocks();
        }
        for (size_t i = 0; i < state.blocks.size(); i++)
        {
            Block& block = state.blocks[i];
            if (block.done)
                continue;
            int64_t when = boot::Resolve(block.at);
            int64_t now = input::ClockMs();
            if (when < 0 || now < when)
                continue;
            block.done = true;
            char chunk[32];
            snprintf(chunk, sizeof(chunk), "dev_lua_%zu", i + 1);
            bool ok = RunLua(ctx, base, block.code.c_str(), chunk);
            Log("[dev] lua block %zu at %s: %s", i + 1, block.atText.c_str(), ok ? "ok" : "error");
            boot::Event(chunk, ok ? "ok" : "error");
        }
    }
}

// CScriptManager::vf10, the per-frame script update: the pending blocks run after it.
PPC_FUNC(sub_82230DE0)
{
    PPCContext saved = ctx;
    __imp__sub_82230DE0(ctx, base);
    dev::RunPendingLua(saved, base);
    dev::console::RunPending(saved, base);
    dev::cheat_menu::RunPending(saved, base);
}

// fputs(text, stream): the guest's stdout/stderr (Lua print) to the log.
PPC_FUNC(sub_823D3410)
{
    uint32_t stream = ctx.r4.u32;
    if (stream == GUEST_STDOUT || stream == GUEST_STDERR)
    {
        WriteStdout(GuestCString(ctx.r3.u32).c_str(), stream == GUEST_STDERR);
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_823D3410(ctx, base);
}



// currentline(ci) (sub_822A1DB0), the line number in Lua error messages: the game's version reads
// through the CallInfo without checks and crashes on some errors raised from C methods (a stale
// ci). This one checks every pointer and answers -1 (no line) when one isn't plausible; it is
// only used to format messages (luaG_runerror -> addinfo).
PPC_FUNC(sub_822A1DB0)
{
    auto plausible = [](uint32_t p) { return (p & 3) == 0 && ((p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000)); };
    int32_t line = -1;
    uint32_t ci = ctx.r3.u32;
    if (plausible(ci))
    {
        uint32_t stackBase = LoadBE32(ci);
        if (plausible(stackBase) && LoadBE32(stackBase - 8) == 6)  // LUA_TFUNCTION
        {
            uint32_t closure = LoadBE32(stackBase - 4);
            if (plausible(closure) && g_memory.Translate(closure)[6] == 0)  // a Lua closure
            {
                uint32_t proto = LoadBE32(closure + 12);
                if (plausible(proto))
                {
                    int32_t pc = int32_t(LoadBE32(ci + 8) - LoadBE32(proto + 12)) / 4 - 1;
                    uint32_t lineinfo = LoadBE32(proto + 52);
                    if (pc >= 0 && pc < 0x100000)
                        line = !lineinfo ? 0 : plausible(lineinfo & ~3u) ? int32_t(__builtin_bswap16(*reinterpret_cast<uint16_t*>(g_memory.Translate(lineinfo + 2 * pc)))) : -1;
                }
            }
        }
    }
    ctx.r3.s64 = line;
}
