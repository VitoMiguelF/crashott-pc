// Menu autopilot (CRASH_MENU_AUTOPILOT): goes through the frontend by itself, pressing each button
// as soon as the screen that takes it is shown, instead of at fixed times. For benchmarks and tests
// that start a level: no wall-clock button script that gets lost when the game is slower or faster
// (a busy machine, an unlocked frame rate), and no waiting for a fixed time on each menu.
//
// The screen shown is the frontend fight tree's node the game evaluates every frame (sub_820F9BA8,
// frontend_hooks.cpp -> NoteFrontendNode). Node numbers are the node order in fighttrees\Frontend.bfig,
// the same in every run (CRASH_FRONTEND_TRACE=1 logs them).
//
//   CRASH_MENU_AUTOPILOT=newgame  logos, title, NEW GAME, name "A", NORMAL, slot 1 (overwrites it),
//                                 CONTINUE: the opening cutscene of episode 1 and then the village.
//   CRASH_MENU_AUTOPILOT=<node>:<button>[:<ms>],...  own steps; <button> as in run.toml (A, START,
//                                 DOWN...); "*" after it repeats it while the node is shown (the
//                                 next step starts when its node shows up); "+" presses it again
//                                 every 1.5 s until the screen changes (a press that came while the
//                                 menu was still animating in is ignored by the game); <ms> = time
//                                 on the screen (or since the previous press) before pressing.
//
// Steps run in order; a step whose node is shown while an earlier one waits (a screen that didn't
// appear, e.g. no "overwrite?" on an empty slot) skips ahead to it. Each press is held 200 ms.

#include "settings.h"
#include "../diagnostics.h"
#include "../kernel/input.h"
#include "../kernel/pc_input.h"
#include "../platform/platform.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    struct Step
    {
        uint32_t node;
        uint16_t button;
        uint32_t delayMs;
        bool repeat;   // "*": every REPEAT_MS while the node is shown
        bool retry;    // "+": again every RETRY_MS until the node changes
    };

    // Measured on the NEW GAME path (README "Desempenho"): 44 logos, 46/59 title, 70 main menu,
    // 96 name, 105 difficulty, 154 save slots, 165 "overwrite?", 320 "save successful".
    constexpr const char* NEW_GAME =
        "44:START*:500,46:START*:300,59:START*:300,70:A+:1500,96:A:2000,96:Y:1500,105:A+:1500,154:A+:1500,165:DOWN:1500,165:A:1000,320:A+:1000";

    constexpr uint32_t HELD_MS = 200;
    constexpr uint32_t REPEAT_MS = 500;
    constexpr uint32_t RETRY_MS = 1500;

    uint16_t ButtonBit(const std::string& name)
    {
        static const struct { const char* name; uint16_t bit; } BUTTONS[] = {
            { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
            { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "UP", XINPUT_GAMEPAD_DPAD_UP },
            { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
            { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
        };
        for (const auto& button : BUTTONS)
            if (name == button.name)
                return button.bit;
        return 0;
    }

    std::vector<Step> Parse(const char* text)
    {
        if (!strcmp(text, "newgame"))
            text = NEW_GAME;
        std::vector<Step> steps;
        std::string all(text);
        size_t start = 0;
        while (start < all.size())
        {
            size_t end = all.find(',', start);
            if (end == std::string::npos)
                end = all.size();
            std::string item = all.substr(start, end - start);
            start = end + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            Step step{ uint32_t(strtoul(item.c_str(), nullptr, 10)), 0, 400, false, false };
            std::string rest = item.substr(colon + 1);
            size_t colon2 = rest.find(':');
            std::string button = rest.substr(0, colon2);
            if (!button.empty() && button.back() == '*')
            {
                step.repeat = true;
                button.pop_back();
            }
            else if (!button.empty() && button.back() == '+')
            {
                step.retry = true;
                button.pop_back();
            }
            step.button = ButtonBit(button);
            if (colon2 != std::string::npos)
                step.delayMs = uint32_t(strtoul(rest.c_str() + colon2 + 1, nullptr, 10));
            if (step.button)
                steps.push_back(step);
            else
                Log("[settings] menu autopilot: unknown button in \"%s\"", item.c_str());
        }
        return steps;
    }

    std::mutex g_mutex;
    std::vector<Step> g_steps;
    size_t g_next = 0;
    std::atomic<uint32_t> g_node{ UINT32_MAX };
    ULONGLONG g_nodeSince = 0;     // when g_node was first seen
    ULONGLONG g_readyFrom = 0;     // the next step's delay counts from here (node change or last press)
    ULONGLONG g_pressUntil = 0;
    uint16_t g_pressButton = 0;
    bool g_lastPressed = false;

    bool Enabled()
    {
        static const bool enabled = [] {
            const char* text = getenv("CRASH_MENU_AUTOPILOT");
            if (!text || !*text)
                return false;
            g_steps = Parse(text);
            // The real cursor over the window would select menu items by hovering (menu_mouse).
            input::pc::UseScriptedMouse();
            Log("[settings] menu autopilot: %zu step(s) (%s); the real mouse is ignored", g_steps.size(), text);
            return !g_steps.empty();
        }();
        return enabled;
    }
}

namespace settings
{
    void MenuAutopilotNode(uint32_t node)
    {
        if (!Enabled() || g_node.load() == node)
            return;
        std::lock_guard lock(g_mutex);
        g_node = node;
        g_nodeSince = g_readyFrom = plat::TickCountMs();
    }

    uint16_t MenuAutopilotButtons()
    {
        if (!Enabled())
            return 0;
        std::lock_guard lock(g_mutex);
        ULONGLONG now = plat::TickCountMs();
        if (now < g_pressUntil)
            return g_pressButton;
        if (g_next >= g_steps.size())
            return 0;
        uint32_t node = g_node;
        // A later step's screen is shown: the ones before it didn't come up.
        for (size_t i = g_next; i < g_steps.size(); i++)
        {
            if (g_steps[i].node != node)
                continue;
            if (i != g_next && !(g_steps[g_next].node == node))
            {
                Log("[settings] menu autopilot: node %u shown, skipping %zu step(s)", node, i - g_next);
                g_next = i;
            }
            break;
        }
        const Step& step = g_steps[g_next];
        if (step.node != node || now < g_readyFrom + step.delayMs)
            return 0;
        g_pressButton = step.button;
        g_pressUntil = now + HELD_MS;
        if (step.repeat)
        {
            // The next press REPEAT_MS after this one is released (a screen only takes a new press).
            g_readyFrom = now + HELD_MS + REPEAT_MS - std::min<ULONGLONG>(step.delayMs, HELD_MS + REPEAT_MS);
        }
        else if (step.retry)
        {
            // Stays on this step: the next step's node showing up moves on (above).
            Log("[settings] menu autopilot: node %u, button 0x%04X (step %zu of %zu, input clock %.1f s)", node, step.button, g_next + 1,
                g_steps.size(), double(input::ClockMs()) / 1000.0);
            g_readyFrom = now + HELD_MS + RETRY_MS - std::min<ULONGLONG>(step.delayMs, HELD_MS + RETRY_MS);
            if (g_next + 1 == g_steps.size() && !g_lastPressed)
            {
                g_lastPressed = true;
                Log("[settings] menu autopilot: last step pressed");
            }
        }
        else
        {
            Log("[settings] menu autopilot: node %u, button 0x%04X (step %zu of %zu, input clock %.1f s)", node, step.button, g_next + 1,
                g_steps.size(), double(input::ClockMs()) / 1000.0);
            g_next++;
            g_readyFrom = now + HELD_MS;
            if (g_next == g_steps.size())
                Log("[settings] menu autopilot: done");
        }
        return g_pressButton;
    }
}
