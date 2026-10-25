#pragma once

// The developer console (dev_console.cpp; README "Console de desenvolvedor", docs/dev_features.md):
// commands typed while playing, run inside the game (dev_tools.h, dev_skins.cpp, dev_cheats.cpp).
//
// Off by default. On with SETTINGS > DEFINITIVE EDITION > DEVELOPER CONSOLE ([dev] console = true in
// settings.toml, live), --dev-console or CRASH_DEV_CONSOLE=1. Then the key left of 1 (` on US
// keyboards, ' on ABNT2; by position, whatever the layout) or F10 opens it; Esc, F10 or that key on
// an empty line closes it. While it is open the game sees a neutral controller and the keyboard
// and mouse type into the console (the game keeps running).
//
// Keys: Enter runs the line, Up / Down walk the history (kept in <userdata>\console_history.txt),
// Tab completes the command or its argument (again: the next match), PageUp / PageDown and the
// wheel scroll the output, Ctrl+V pastes, Ctrl+L clears the output, Ctrl+C / Ctrl+U clears the line.
//
// Commands (help lists them): unlock all, unlock skin <name>, mojo <n>, god on|off, tp x y z, pos,
// shadow on|off, bighead on|off, skin [<name> [now]], level [<name>], reload, lua <code> (lua
// =<expr> prints the value), clear, help [command]. Lua's devprint output and Lua errors show up
// in the console as well as in the log ("[console] ..." lines).
//
// Tests: CRASH_DEV_CONSOLE_SCRIPT=<file>, lines "<time>|<entry>" (<time> as CRASH_DEV_LUA_AT:
// "<event>[#n][+seconds]" or seconds); an entry is typed into the window as keys (window
// messages, the same path as a real keyboard): "!toggle" (the key left of 1), "!esc", "!tab",
// "!up", "!down", "!enter", "!type:<text>" (no Enter), or a command (typed, then Enter). Each entry
// logs the event "console_<n>"; each command run logs "console_cmd" (detail: the command).
// tools/run_game.py: dev_console = [["@gameplay_ready+1", "!toggle"], [..., "god on"], ...].

#include <cstdint>
#include <string>

struct PPCContext;

namespace dev::console
{
    // main(): --dev-console.
    void HandleArguments(int argc, char** argv);
    // The console can be opened (setting, argument or environment).
    bool Enabled();
    bool IsOpen();
    // The game must see a neutral controller: open, or closed less than a moment ago.
    bool BlocksGameInput();

    // ---- window thread (gpu/window.cpp) ----
    // WM_KEYDOWN / WM_SYSKEYDOWN, WM_CHAR, WM_MOUSEWHEEL: true when the console took it (it must
    // not reach the game). `key` / `lParam` are the messages' WPARAM / LPARAM.
    bool OnKeyDown(uintptr_t key, intptr_t lParam);
    bool OnChar(wchar_t c, intptr_t lParam, bool dead = false);  // dead: WM_DEADCHAR
    bool OnWheel(int notches);

    // ---- game thread: once a frame, after the script update (dev_tools.cpp) ----
    void RunPending(PPCContext& ctx, uint8_t* base);
    // The frontend's condition function (settings/frontend_hooks.cpp): "level <name>" asked outside a
    // level starts it from the HUB (575) or the main menu (70): Frontend:StartLevel + the tree's
    // SkipToLevel exit (51), as the direct boot does. Returns the exit to take.
    int32_t OverrideExit(PPCContext& ctx, uint8_t* base, uint32_t tree, uint32_t node, int32_t exit);

    // ---- output, any thread ----
    enum class Tone : uint8_t { Normal, Input, Ok, Error, Lua, Help, Dim };
    void Print(const std::string& text, Tone tone = Tone::Normal);

    // ---- render thread, once per presented frame, over the back buffer (RENDER_TARGET) ----
    void Draw(uint32_t width, uint32_t height);
}
