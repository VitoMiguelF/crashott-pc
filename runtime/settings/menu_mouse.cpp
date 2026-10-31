// The frontend's menus as seen by the mouse (menu_mouse.h).

#include "menu_mouse.h"
#include "../fs/rcf_overlay.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "settings.h"
#include "settings_menu.h"
#include "save_screens.h"
#include "../online/mp_menu.h"
#include "../platform/platform.h"
#include "../gpu/widescreen.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

extern "C" PPC_FUNC(__imp__sub_820B2E98);  // CMenuAction::vf05: the frontend tree's menu action, once per frame (this+24 = the menu)
extern "C" PPC_FUNC(__imp__sub_820BE0C0);  // another menu action's update (this+24..36 = its menus; the overwrite question of NEW GAME)
extern "C" PPC_FUNC(__imp__sub_821D5858);  // CConfirmMessage::vf03: a YES / NO question's update, once per frame (this+16 = its menu)
PPC_EXTERN_FUNC(sub_822F2E40);  // Menu::SetSelection(menu, index)
PPC_EXTERN_FUNC(sub_822F2F58);  // Menu: previous selectable item (returns 1 if it moved)
PPC_EXTERN_FUNC(sub_822F3068);  // Menu: next selectable item
PPC_EXTERN_FUNC(sub_821FB4F0);  // the managers; sub_821FA440(them, 6) = the frontend sound player
PPC_EXTERN_FUNC(sub_821FA440);
PPC_EXTERN_FUNC(sub_821DB260);  // the menu action's "previous" move sound
PPC_EXTERN_FUNC(sub_821DB280);  // its "next" move sound

namespace input::menu_mouse
{
    namespace
    {
        constexpr uint32_t FE_PAGE = 0x18002, FE_TEXT = 0x18023;

        std::mutex g_mutex;
        // Text boxes of every frontend page: (x, y) of the bottom-left corner -> (w, h), 640x480 y up.
        std::multimap<std::pair<int, int>, std::pair<int, int>> g_boxes;
        Menu g_live;
        uint64_t g_liveTime = 0;
        // A YES / NO question (CConfirmMessage) was on screen at this time: the menu behind it (the
        // pause list under "SAVE AND EXIT?") still runs, but the mouse is the question's.
        std::atomic<uint64_t> g_confirmTime{ 0 };
        // The last frame with the frontend's page drawn through its camera (NoteFrontendPage).
        std::atomic<uint64_t> g_pageTime{ 0 };

        uint32_t LE32(const std::vector<uint8_t>& d, size_t at)
        {
            return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
        }

        // Walks the chunks from `chunk` to `end`; `inPage`: below a page.
        void Collect(const std::vector<uint8_t>& d, size_t chunk, size_t end, bool inPage)
        {
            while (chunk + 12 <= end)
            {
                uint32_t id = LE32(d, chunk), header = LE32(d, chunk + 4), total = LE32(d, chunk + 8);
                if (header < 12 || total < header || chunk + total > end)
                    return;
                if (inPage && id == FE_TEXT && header > 13)
                {
                    size_t tail = chunk + 13 + d[chunk + 12];
                    if (tail + 20 <= chunk + header)
                    {
                        int x = int(LE32(d, tail + 4)), y = int(LE32(d, tail + 8)), w = int(LE32(d, tail + 12)), h = int(LE32(d, tail + 16));
                        if (w > 0 && h > 0 && w <= 1280 && h <= 960)
                            g_boxes.insert({ { x, y }, { w, h } });
                    }
                }
                Collect(d, chunk + header, chunk + total, inPage || id == FE_PAGE);
                chunk += total;
            }
        }

        bool ReadBoxes(std::vector<uint8_t>& data)
        {
            std::lock_guard lock(g_mutex);
            if (data.size() >= 12)
                Collect(data, LE32(data, 4), std::min<size_t>(data.size(), LE32(data, 8)), false);
            return false;  // read only
        }

        // The width and height of the text box at (x, y): exact match, else the nearest box with
        // the same x (items moved up or down by a patch), else a typical menu item.
        std::pair<float, float> BoxSize(float x, float y)
        {
            int ix = int(std::lround(x)), iy = int(std::lround(y));
            auto exact = g_boxes.find({ ix, iy });
            if (exact != g_boxes.end())
                return { float(exact->second.first), float(exact->second.second) };
            std::pair<int, int> best{ 200, 41 };
            int bestDistance = INT_MAX;
            for (auto it = g_boxes.lower_bound({ ix, INT_MIN }); it != g_boxes.end() && it->first.first == ix; ++it)
            {
                int distance = std::abs(it->first.second - iy);
                if (distance < bestDistance)
                    bestDistance = distance, best = it->second;
            }
            return { float(best.first), float(best.second) };
        }
    }

    void Init()
    {
        // The packages with frontend pages (main menu and save screens; boot; in-game pause, end
        // of episode, messages).
        for (const char* package : { "cdd70a8c", "b9c6f232", "b4c85fe7", "1725c617", "7efdcd91", "9eed48ad", "79290aaa", "c1e387c7",
                                     "aabeaa37", "ad83ca09", "7a83cb8b", "97597d1b" })
            rcf_overlay::RegisterBuiltinPatch("default", std::string("package\\") + package + ".p3d", "caixas dos menus para o mouse (so leitura)",
                                              ReadBoxes);
    }

    void NoteFrontendPage()
    {
        g_pageTime.store(plat::TickCountMs(), std::memory_order_relaxed);
    }

    float PageScale()
    {
        uint64_t frontend = g_pageTime.load(std::memory_order_relaxed);
        bool wide = fabsf(gpu::widescreen::Aspect() - gpu::widescreen::BASE_ASPECT) >= 0.001f;
        return frontend && plat::TickCountMs() - frontend <= 250 && wide ? 1.5f : 2.0f;
    }

    bool Live(Menu& menu)
    {
        std::lock_guard lock(g_mutex);
        if (!g_liveTime || plat::TickCountMs() - g_liveTime > 150)
            return false;
        menu = g_live;
        return true;
    }

    int HitTest(const Menu& menu, float x, float y)
    {
        // A row of items (the save slots) is usually a label under a picture: the area above the
        // label counts too. A column keeps a little room at the sides.
        for (size_t i = 0; i < menu.items.size(); i++)
        {
            const Item& item = menu.items[i];
            float left = item.x0, right = item.x1, bottom = item.y0, top = item.y1;
            if (menu.vertical)
                left -= 16, right += 16;
            else
                bottom -= 24, top += 200;
            if (item.x1 > item.x0 && x >= left && x < right && y >= bottom && y < top)
                return int(i);
        }
        return -1;
    }
}

namespace
{
    using namespace input::menu_mouse;

    // Menu::SetSelection(index) at once, and the move sound the menu action plays for previous
    // (sub_821DB260) or next (sub_821DB280): the sound player is sub_821FA440(sub_821FB4F0(), 6).
    void Select(PPCContext& ctx, uint8_t* base, uint32_t menu, int index, bool forward)
    {
        PPCContext saved = ctx;
        ctx.r3.u64 = menu;
        ctx.r4.s64 = index;
        sub_822F2E40(ctx, base);
        sub_821FB4F0(ctx, base);
        ctx.r4.s64 = 6;
        sub_821FA440(ctx, base);
        if (forward)
            sub_821DB280(ctx, base);
        else
            sub_821DB260(ctx, base);
        ctx = saved;
    }

    // The game's previous / next (one wheel notch), with its sound when the selection moved.
    void Step(PPCContext& ctx, uint8_t* base, uint32_t menu, bool forward)
    {
        PPCContext saved = ctx;
        ctx.r3.u64 = menu;
        if (forward)
            sub_822F3068(ctx, base);
        else
            sub_822F2F58(ctx, base);
        if (ctx.r3.u32 & 0xFF)
        {
            sub_821FB4F0(ctx, base);
            ctx.r4.s64 = 6;
            sub_821FA440(ctx, base);
            if (forward)
                sub_821DB280(ctx, base);
            else
                sub_821DB260(ctx, base);
        }
        ctx = saved;
    }

    // The cursor in page units (640 x 480, y up): the page stretched over the picture's 16:9 area
    // (1280 x 720), or on a frontend screen of a picture that is not 16:9, 4:3 in it
    // (NoteFrontendPage).
    bool CursorOnPage(float& x, float& y)
    {
        float u, v;
        if (!input::pc::CursorOnImage(u, v))
            return false;
        float page = PageScale();  // image pixels per page unit across
        x = 320.0f + (u - 640.0f) / page, y = (720.0f - v) * (480.0f / 720.0f);
        return true;
    }

    // Hover, click and wheel on the live menu (game thread, once per frame, after the menu action
    // read the pads and the keyboard).
    void MouseOnMenu(PPCContext& ctx, uint8_t* base, const Menu& menu)
    {
        static uint32_t s_menu = 0, s_moves = 0;
        uint32_t moves = input::pc::CursorMoves();
        bool moved = moves != s_moves;
        s_moves = moves;
        if (settings::menu::BlocksGameInput() || settings::save_screens::BlocksMenuMouse() ||
            mp::menu::BlocksGameInput())
        {
            // The settings screen (drawn over the PS2 options menu, which also runs) has the mouse.
            input::pc::TakeMenuClick();
            input::pc::TakeMenuWheel();
            s_menu = 0;
            return;
        }
        if (menu.id != s_menu)
        {
            // A new menu: the cursor's place is not a hover until it moves again.
            s_menu = menu.id;
            moved = false;
            input::pc::TakeMenuWheel();
        }
        float x = -1, y = -1;
        int hit = -1;
        if (CursorOnPage(x, y))
            hit = HitTest(menu, x, y);
        bool usable = hit >= 0 && menu.items[size_t(hit)].enabled;
        static const bool trace = getenv("CRASH_MOUSE_TRACE") != nullptr;
        if (moved && usable && hit != menu.index)
        {
            Log("[input] mouse: hover menu 0x%08X item %d -> selected (was %d)", menu.id, hit, menu.index);
            Select(ctx, base, menu.id, hit, hit > menu.index);
        }
        if (input::pc::TakeMenuClick())
        {
            if (usable)
            {
                if (hit != int(int8_t(*(base + menu.id + 152))))
                    Select(ctx, base, menu.id, hit, hit > menu.index);
                Log("[input] mouse: click menu 0x%08X item %d -> accept", menu.id, hit);
                input::pc::PressMenuAccept();
            }
            else if (trace)
                Log("[input] mouse: click menu 0x%08X outside the items (page %.0f,%.0f)", menu.id, x, y);
        }
        int wheel = input::pc::TakeMenuWheel();
        if (wheel)
        {
            int before = int(int8_t(*(base + menu.id + 152)));
            // Wheel up (away from the user) = the item before (above / to the left).
            for (int i = 0; i < std::min(std::abs(wheel), 8); i++)
                Step(ctx, base, menu.id, wheel < 0);
            Log("[input] mouse: wheel %+d on menu 0x%08X: item %d -> %d", wheel, menu.id, before, int(int8_t(*(base + menu.id + 152))));
        }
    }

    // The menu at `menu` was just updated by the game: keeps it as the live menu and lets the
    // mouse act on it.
    void OnMenuUpdate(PPCContext& ctx, uint8_t* base, uint32_t menu)
    {
        uint32_t begin = LoadBE32(menu + 112), end = LoadBE32(menu + 116);
        if (end < begin || end - begin > 4 * 64)
            return;
        Menu live;
        live.id = menu;
        live.index = int(int8_t(*(base + menu + 152)));
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        bool anyEnabled = false;
        {
            std::lock_guard lock(g_mutex);
            for (uint32_t at = begin; at < end; at += 4)
            {
                uint32_t item = LoadBE32(at), text = item ? LoadBE32(item + 140) : 0;
                Item box{ 0, 0, 0, 0 };
                box.enabled = item && (*(base + item + 137) & 0x80) != 0;
                anyEnabled |= box.enabled;
                if (text)
                {
                    float fx, fy;
                    uint32_t vx = LoadBE32(text + 92), vy = LoadBE32(text + 96);
                    memcpy(&fx, &vx, 4);
                    memcpy(&fy, &vy, 4);
                    float x = fx * 640.0f, y = fy * 640.0f;
                    if (std::isfinite(x) && std::isfinite(y) && x > -640 && x < 1280 && y > -480 && y < 960)
                    {
                        auto [w, h] = BoxSize(x, y);
                        box.x0 = x, box.y0 = y, box.x1 = x + w, box.y1 = y + h;
                        minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
                    }
                }
                live.items.push_back(box);
            }
        }
        if (!anyEnabled)
        {
            // A menu without the "selectable" bits (its previous / next do not look at them).
            for (Item& item : live.items)
                item.enabled = true;
        }
        live.vertical = (maxY - minY) >= (maxX - minX);
        static const bool trace = getenv("CRASH_MOUSE_TRACE") != nullptr;
        if (trace)
        {
            static std::map<uint32_t, uint64_t> s_seen;
            static std::map<uint32_t, int> s_index;
            uint64_t now = plat::TickCountMs();
            auto& last = s_seen[menu];
            if (now - last > 2000)
            {
                std::string boxes;
                for (const Item& item : live.items)
                    boxes += " [" + std::to_string(int(item.x0)) + "," + std::to_string(int(item.y0)) + " " + std::to_string(int(item.x1 - item.x0)) + "x" +
                             std::to_string(int(item.y1 - item.y0)) + (item.enabled ? "" : " off") + "]";
                Log("[menu] update 0x%08X (%s): %zu items, index %d:%s", menu, live.vertical ? "column" : "row", live.items.size(), live.index,
                    boxes.c_str());
            }
            last = now;
            // Every selection change, whoever made it (one line per hover; one per step for the pads).
            auto previous = s_index.find(menu);
            if (previous != s_index.end() && previous->second != live.index)
                Log("[menu] 0x%08X selection %d -> %d", menu, previous->second, live.index);
            s_index[menu] = live.index;
        }
        {
            std::lock_guard lock(g_mutex);
            g_live = live;
            g_liveTime = plat::TickCountMs();
        }
        MouseOnMenu(ctx, base, live);
    }
}

PPC_FUNC(sub_820B2E98)
{
    uint32_t action = ctx.r3.u32;
    __imp__sub_820B2E98(ctx, base);
    uint32_t menu = LoadBE32(action + 24);
    if (!menu || plat::TickCountMs() - g_confirmTime.load(std::memory_order_relaxed) <= 150)
        return;
    uint32_t result = ctx.r3.u32;
    OnMenuUpdate(ctx, base, menu);
    ctx.r3.u64 = result;  // the action's own result
}

// A YES / NO question ("Are you sure you want to save the game and exit the level?"): its own menu
// (this+16, the same pure3d::frontend::Menu) reads LEFT / RIGHT and A in this update while byte +4
// has 0x80 (shown) and 0x40 (taking input); the menu action behind it keeps running.
PPC_FUNC(sub_821D5858)
{
    uint32_t message = ctx.r3.u32;
    __imp__sub_821D5858(ctx, base);
    uint32_t menu = LoadBE32(message + 16);
    if (!menu || (*(base + message + 4) & 0xC0) != 0xC0)
        return;
    g_confirmTime.store(plat::TickCountMs(), std::memory_order_relaxed);
    OnMenuUpdate(ctx, base, menu);
}

// Another menu action (NEW GAME's "overwrite this saved game?" NO / YES, which runs instead of the
// slot screen's CMenuAction::vf05): four menus at this+24 / 28 / 32 / 36, the one of step s
// (1..4, (this+8)+36) at this+20+4s reads UP / DOWN and A and is updated (its vf20) each frame.
// It is the live menu while that step's menu is there; the action returns 1.
PPC_FUNC(sub_820BE0C0)
{
    uint32_t action = ctx.r3.u32;
    __imp__sub_820BE0C0(ctx, base);
    uint32_t result = ctx.r3.u32;
    uint32_t owner = LoadBE32(action + 8);
    if (!LoadBE32(action + 20) || !owner)
        return;
    uint32_t step = LoadBE32(owner + 36);
    uint32_t menu = step >= 1 && step <= 4 ? LoadBE32(action + 20 + 4 * step) : 0;
    if (!menu || plat::TickCountMs() - g_confirmTime.load(std::memory_order_relaxed) <= 150)
        return;
    OnMenuUpdate(ctx, base, menu);
    ctx.r3.u64 = result;
}
