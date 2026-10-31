// The achievements (achievements.h): the SPA read from the loaded image, the unlocked list in
// <userdata>/achievements.toml, the XGI write message, the screen's input on the game thread and
// its sounds. The drawing is in achievements_draw.cpp.

#include "achievements_internal.h"
#include "../dev/cheat_menu.h"
#include "../dev/dev_console.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "../online/mp_menu.h"
#include "../paths.h"
#include "../platform/image.h"
#include "../platform/keys.h"
#include "../platform/platform.h"
#include "../settings/controls.h"
#include "../settings/direct_boot.h"
#include "../settings/settings.h"
#include "../settings/settings_menu.h"
#include "../settings/ui_text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <sstream>

PPC_EXTERN_FUNC(sub_821FB4F0);  // the managers; sub_821FA440(them, n) = subsystem n (6 the frontend sound player, 10 the objectives)
PPC_EXTERN_FUNC(sub_821FA440);
PPC_EXTERN_FUNC(sub_821DB1D8);  // sound player: play (bank r4, sound r5, volume f1)
PPC_EXTERN_FUNC(sub_821DB260);  // the menus' "previous" move sound
PPC_EXTERN_FUNC(sub_821DB280);  // the menus' "next" move sound
PPC_EXTERN_FUNC(sub_8220A708);  // CObjectiveManager: queue XUSER_ACHIEVEMENTs (r4, count r5) for XUserWriteAchievements

namespace achievements
{
    using namespace detail;

    namespace
    {
        // ---- the SPA (XDBF) ----------------------------------------------------------------------
        // XDBF: header { 'XDBF', version, entry table length, entry count, free table length, free
        // count }, then the entries { u16 namespace, u64 id, u32 offset, u32 length } (18 bytes),
        // the free table (8 bytes each) and the data. Namespace 1 = metadata ('XACH' the
        // achievements, 'XSTC' the default language), 2 = images (PNG, by image id), 3 = string
        // tables 'XSTR' (by language: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian).
        // 'XACH' / 'XSTR': { magic, version, size, u16 count } then the records; an achievement is
        // 36 bytes { u16 id, label, description, unachieved; u32 image; u16 gamerscore, ?; u32 flags; 16 ? }
        // (flags 0x8 = shown while locked; without it the achievement is secret).
        constexpr uint32_t XEX_HEADER_RESOURCE_INFO = 0x000002FF;
        constexpr uint64_t XACH = 0x58414348, XSTC = 0x58535443;
        constexpr uint32_t LANGUAGE_ENGLISH = 1, LANGUAGE_GERMAN = 3, LANGUAGE_FRENCH = 4, LANGUAGE_SPANISH = 5, LANGUAGE_ITALIAN = 6;

        uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

        // The SPA's address and size in the image, from default.xex's plaintext header.
        bool FindSpa(uint32_t& address, uint32_t& size)
        {
            std::ifstream file(paths::Xex(), std::ios::binary);
            std::vector<uint8_t> header(0x18);
            if (!file.read(reinterpret_cast<char*>(header.data()), header.size()) || memcmp(header.data(), "XEX2", 4) != 0)
                return false;
            uint32_t headerSize = Be32(&header[0x08]);
            if (headerSize < 0x18 || headerSize > 0x1000000)
                return false;
            header.resize(headerSize);
            if (!file.read(reinterpret_cast<char*>(header.data() + 0x18), headerSize - 0x18))
                return false;
            uint32_t count = Be32(&header[0x14]);
            for (uint32_t i = 0; i < count && 0x18 + i * 8 + 8 <= headerSize; i++)
            {
                if (Be32(&header[0x18 + i * 8]) != XEX_HEADER_RESOURCE_INFO)
                    continue;
                uint32_t at = Be32(&header[0x18 + i * 8 + 4]);
                if (at + 4 > headerSize)
                    return false;
                uint32_t bytes = Be32(&header[at]);
                for (uint32_t r = 0; r + 1 <= (bytes - 4) / 16 && at + 4 + r * 16 + 16 <= headerSize; r++)
                {
                    const uint8_t* entry = &header[at + 4 + r * 16];
                    uint32_t a = Be32(entry + 8), s = Be32(entry + 12);
                    if (s >= 24 && a >= PPC_IMAGE_BASE && memcmp(g_memory.Translate(a), "XDBF", 4) == 0)
                    {
                        address = a, size = s;
                        return true;
                    }
                }
            }
            return false;
        }

        struct Xdbf
        {
            const uint8_t* data = nullptr;
            uint32_t size = 0;
            struct Entry
            {
                uint16_t space;
                uint64_t id;
                uint32_t offset, length;
            };
            std::vector<Entry> entries;
            uint32_t dataStart = 0;

            bool Parse(const uint8_t* d, uint32_t s)
            {
                data = d, size = s;
                uint32_t tableLength = Be32(d + 8), count = Be32(d + 12), freeLength = Be32(d + 16);
                dataStart = 24 + tableLength * 18 + freeLength * 8;
                if (count > tableLength || dataStart > size)
                    return false;
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint8_t* e = d + 24 + i * 18;
                    entries.push_back({ Be16(e), Be64(e + 2), Be32(e + 10), Be32(e + 14) });
                }
                return true;
            }

            // A section's bytes (nullptr if absent or out of bounds).
            const uint8_t* Find(uint16_t space, uint64_t id, uint32_t& length) const
            {
                for (const Entry& e : entries)
                {
                    if (e.space == space && e.id == id && uint64_t(dataStart) + e.offset + e.length <= size)
                    {
                        length = e.length;
                        return data + dataStart + e.offset;
                    }
                }
                return nullptr;
            }
        };

        std::map<uint16_t, std::string> Strings(const Xdbf& xdbf, uint32_t language)
        {
            std::map<uint16_t, std::string> strings;
            uint32_t length;
            const uint8_t* table = xdbf.Find(3, language, length);
            if (!table || length < 14 || memcmp(table, "XSTR", 4) != 0)
                return strings;
            uint32_t at = 14;
            for (uint16_t i = 0, count = Be16(table + 12); i < count && at + 4 <= length; i++)
            {
                uint16_t id = Be16(table + at), bytes = Be16(table + at + 2);
                at += 4;
                if (at + bytes > length)
                    break;
                strings[id].assign(reinterpret_cast<const char*>(table + at), bytes);
                at += bytes;
            }
            return strings;
        }

        uint32_t WantedLanguage(settings::UiLanguage language)
        {
            switch (language)
            {
            case settings::UiLanguage::French: return LANGUAGE_FRENCH;
            case settings::UiLanguage::German: return LANGUAGE_GERMAN;
            case settings::UiLanguage::Spanish: return LANGUAGE_SPANISH;
            case settings::UiLanguage::Italian: return LANGUAGE_ITALIAN;
            default: return LANGUAGE_ENGLISH;
            }
        }

        bool DecodeIcon(const uint8_t* png, uint32_t length, settings::menu_art::Image& icon, settings::menu_art::Image& locked)
        {
            int w, h;
            std::vector<uint8_t> rgba;
            if (!plat::DecodePng(png, length, w, h, rgba) || w <= 0 || h <= 0)
                return false;
            icon.w = locked.w = w, icon.h = locked.h = h;
            icon.px.resize(size_t(w) * h);
            locked.px.resize(icon.px.size());
            for (size_t i = 0; i < icon.px.size(); i++)
            {
                uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
                auto pm = [&](uint32_t c) { return (c * a + 127) / 255; };
                icon.px[i] = pm(r) | pm(g) << 8 | pm(b) << 16 | a << 24;
                // Grey, darker and a little flat: what the dashboard showed for a locked one.
                uint32_t grey = (r * 77 + g * 151 + b * 28) >> 8;
                grey = 28 + grey * 40 / 100;
                locked.px[i] = pm(grey) | pm(grey) << 8 | pm(grey) << 16 | a << 24;
            }
            return true;
        }

        Data LoadData(settings::UiLanguage uiLanguage)
        {
            Data data;
            uint32_t address = 0, size = 0;
            if (!FindSpa(address, size))
            {
                Log("[achievements] no XDBF resource in %s", paths::Xex().string().c_str());
                return data;
            }
            Xdbf xdbf;
            uint32_t length = 0;
            const uint8_t* xach = nullptr;
            if (!xdbf.Parse(g_memory.Translate(address), size) || !(xach = xdbf.Find(1, XACH, length)) || length < 14)
            {
                Log("[achievements] the XDBF at 0x%08X has no achievements", address);
                return data;
            }
            uint32_t language = WantedLanguage(uiLanguage);
            std::map<uint16_t, std::string> strings = Strings(xdbf, language);
            if (strings.empty())
            {
                uint32_t xstcLength;
                const uint8_t* xstc = xdbf.Find(1, XSTC, xstcLength);
                language = LANGUAGE_ENGLISH;
                strings = Strings(xdbf, language);
                if (strings.empty() && xstc && xstcLength >= 16)
                    strings = Strings(xdbf, language = Be32(xstc + 12));
            }
            auto text = [&](uint16_t id) {
                auto it = strings.find(id);
                return it == strings.end() ? std::string() : it->second;
            };
            uint16_t count = Be16(xach + 12);
            for (uint16_t i = 0; i < count && 14 + (i + 1) * 36u <= length; i++)
            {
                const uint8_t* r = xach + 14 + i * 36;
                Achievement a;
                a.id = Be16(r);
                a.name = text(Be16(r + 2));
                a.description = text(Be16(r + 4));
                a.howTo = text(Be16(r + 6));
                a.gamerscore = Be16(r + 12);
                a.secret = (Be32(r + 16) & 0x8) == 0;
                uint32_t pngLength;
                if (const uint8_t* png = xdbf.Find(2, Be32(r + 8), pngLength))
                    DecodeIcon(png, pngLength, a.icon, a.iconLocked);
                data.totalScore += a.gamerscore;
                data.list.push_back(std::move(a));
            }
            data.ok = !data.list.empty();
            Log("[achievements] %zu achievements, %d gamerscore, language %u (SPA at 0x%08X, %u bytes)", data.list.size(), data.totalScore, language,
                address, size);
            return data;
        }

        // ---- the unlocked list -------------------------------------------------------------------
        std::mutex g_mutex;  // everything below
        std::map<uint16_t, std::string> g_unlocked;
        uint64_t g_unlockedVersion = 0;
        bool g_unlockedLoaded = false;
        std::deque<uint16_t> g_popups;
        std::unique_ptr<Data> g_previewData;  // --achievements-preview: the SPA in each language

        std::filesystem::path StoreFile()
        {
            return vfs::UserDataDir() / "achievements.toml";
        }

        void LoadUnlockedLocked()
        {
            if (g_unlockedLoaded)
                return;
            g_unlockedLoaded = true;
            std::ifstream file(StoreFile());
            std::string line;
            while (std::getline(file, line))
            {
                unsigned id;
                char date[64] = "";
                if (sscanf(line.c_str(), " %u = \"%63[^\"]\"", &id, date) == 2 && id < 0x10000)
                    g_unlocked[uint16_t(id)] = date;
            }
            if (!g_unlocked.empty())
                Log("[achievements] %zu unlocked in %s", g_unlocked.size(), StoreFile().string().c_str());
        }

        void SaveUnlockedLocked()
        {
            const Data& data = GetData();
            std::ostringstream out;
            out << "# CrashRecomp: the Xbox 360 achievements unlocked here. They count for every save slot.\n"
                   "# id = \"date and time unlocked\" (local time). Delete a line to lock it again.\n"
                   "[unlocked]\n";
            for (const auto& [id, date] : g_unlocked)
            {
                out << id << " = \"" << date << "\"";
                auto it = std::find_if(data.list.begin(), data.list.end(), [&](const Achievement& a) { return a.id == id; });
                if (it != data.list.end() && it->name.find('\n') == std::string::npos)
                    out << "  # " << it->name;
                out << "\n";
            }
            std::filesystem::path path = StoreFile(), temporary = path;
            temporary += ".tmp";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                file << out.str();
                if (!file)
                {
                    Log("[achievements] could not write %s", temporary.string().c_str());
                    return;
                }
            }
            std::error_code error;
            std::filesystem::rename(temporary, path, error);
            if (error)
                Log("[achievements] could not write %s: %s", path.string().c_str(), error.message().c_str());
        }

        std::string Now()
        {
            std::time_t t = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
            return text;
        }

        // ---- the screen (game thread) ------------------------------------------------------------
        enum class Action { Open, Back, Next, Previous, Unlock };

        std::deque<Action> g_actions;  // under g_mutex
        std::vector<HitZone> g_zones, g_hintZones;  // under g_mutex
        Visual g_visual;  // under g_mutex

        std::atomic<bool> g_open{ false }, g_releaseHold{ false }, g_hintWanted{ false };
        int g_selected = 0;
        uint16_t g_lastButtons = 0, g_heldDir = 0;
        uint64_t g_heldSince = 0, g_lastRepeat = 0, g_closedAt = 0;
        uint32_t g_mouseMoves = 0, g_mouseClicks = 0;
        bool g_keyHeld = false;
        uint64_t g_keysReadAt = 0;
        std::array<uint16_t, settings::KEYS_PER_ACTION> g_mapKeys{};

        constexpr uint16_t DIRECTIONS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
        constexpr uint64_t RELEASE_MS = 150;
        constexpr uint32_t NODE_MAIN_MENU = 70, NODE_HUB = 575;

        void Queue(Action action)
        {
            std::lock_guard lock(g_mutex);
            g_actions.push_back(action);
        }

        void Publish()
        {
            std::lock_guard lock(g_mutex);
            g_visual.open = g_open;
            g_visual.selected = g_selected;
        }

        // The frontend tree's pause screens (docs/internals/frontend-and-boot.md): 1081 the pause,
        // 1109 the pause without saving, 1134 the demo's.
        bool IsPause(uint32_t node) { return node == 1081 || node == 1109 || node == 1134; }
        bool HintNode(uint32_t node) { return node == NODE_MAIN_MENU || IsPause(node); }
        bool OpenNode(uint32_t node) { return HintNode(node) || node == NODE_HUB; }

        // Another screen of the runtime has the controller.
        bool OtherScreen()
        {
            return settings::menu::BlocksGameInput() || dev::console::BlocksGameInput() || mp::menu::BlocksGameInput() ||
                   dev::cheat_menu::BlocksGameInput();
        }

        bool ZoneAt(const std::vector<HitZone>& zones, float x, float y, HitZone& out)
        {
            for (const HitZone& zone : zones)
            {
                if (x >= zone.x0 && x < zone.x1 && y >= zone.y0 && y < zone.y1)
                {
                    out = zone;
                    return true;
                }
            }
            return false;
        }

        void Open(const char* how)
        {
            g_open = true;
            g_selected = 0;
            g_heldDir = 0;
            g_mouseMoves = input::pc::CursorMoves(), g_mouseClicks = input::pc::LeftClicks();
            Log("[achievements] screen opened (%s)", how);
            boot::Event("achievements_screen");
            Queue(Action::Open);
            Publish();
        }

        void Close(const char* why)
        {
            g_open = false;
            g_releaseHold = true;
            g_closedAt = plat::TickCountMs();
            Log("[achievements] screen closed (%s)", why);
            Publish();
        }

        void Select(int index)
        {
            int count = int(GetData().list.size());
            if (index < 0 || index >= count || index == g_selected)
                return;
            Queue(index > g_selected ? Action::Next : Action::Previous);
            g_selected = index;
        }

        // The keyboard's BACK (the Map action's keys, Tab by default): in the menus it is not BACK
        // (Tab is the menus' Y), so it is read here, on the main menu and the pause, where Y does
        // nothing. On the HUB Tab is CONTINUE STORY: only another key opens there.
        bool MapKeyPressed(uint32_t node, uint64_t now)
        {
            if (now - g_keysReadAt > 1000)
            {
                g_mapKeys = settings::Get().controls.keys[size_t(settings::Action::Map)];
                g_keysReadAt = now;
            }
            bool held = false;
            for (uint16_t key : g_mapKeys)
            {
                bool menuKey = key == VK_TAB || key == VK_ESCAPE || key == VK_RETURN || key == VK_SPACE || key == VK_BACK;
                if (key && key < 256 && input::pc::KeyHeld(key) && (node != NODE_HUB || !menuKey))
                    held = true;
            }
            bool pressed = held && !g_keyHeld;
            g_keyHeld = held;
            return pressed;
        }

        void WatchOpen(uint16_t pressed, uint64_t now)
        {
            uint32_t node = settings::menu::FrontendNode();
            bool where = OpenNode(node) && GetData().ok && !OtherScreen();
            g_hintWanted = where && HintNode(node);
            bool key = MapKeyPressed(node, now);
            uint32_t clicks = input::pc::LeftClicks();
            bool clicked = clicks != g_mouseClicks;
            g_mouseClicks = clicks;
            if (!where)
                return;
            if ((pressed & XINPUT_GAMEPAD_BACK) && !(mp::menu::HiddenButtons() & XINPUT_GAMEPAD_BACK))
                Open("BACK");
            else if (key)
                Open("keyboard");
            else if (clicked && g_hintWanted)
            {
                float x, y;
                HitZone zone{};
                bool onHint;
                {
                    std::lock_guard lock(g_mutex);
                    onHint = input::pc::CursorOnImage(x, y) && ZoneAt(g_hintZones, x, y, zone);
                }
                if (onHint)
                {
                    input::pc::TakeMenuClick();
                    Open("click on the hint");
                }
            }
        }

        // The screen's input (open).
        void Drive(uint16_t buttons, uint16_t pressed, uint64_t now)
        {
            uint32_t node = settings::menu::FrontendNode();
            if (!OpenNode(node))
            {
                Close("the menu changed");
                return;
            }
            const int count = int(GetData().list.size());
            uint16_t dir = buttons & DIRECTIONS;
            uint16_t step = 0;
            if (dir && dir != g_heldDir)
            {
                step = dir;
                g_heldSince = g_lastRepeat = now;
            }
            else if (dir && now - g_heldSince > 400 && now - g_lastRepeat > 110)
            {
                step = dir;
                g_lastRepeat = now;
            }
            g_heldDir = dir;
            if (step & XINPUT_GAMEPAD_DPAD_LEFT)
                Select(g_selected > 0 ? g_selected - 1 : count - 1);
            else if (step & XINPUT_GAMEPAD_DPAD_RIGHT)
                Select(g_selected + 1 < count ? g_selected + 1 : 0);
            else if (step & XINPUT_GAMEPAD_DPAD_UP)
                Select(g_selected >= COLUMNS ? g_selected - COLUMNS : std::min(count - 1, (count - 1) / COLUMNS * COLUMNS + g_selected % COLUMNS));
            else if (step & XINPUT_GAMEPAD_DPAD_DOWN)
                Select(g_selected + COLUMNS < count ? g_selected + COLUMNS : g_selected % COLUMNS);

            // The mouse / the touch screen: pointing selects.
            uint32_t moves = input::pc::CursorMoves(), clicks = input::pc::LeftClicks();
            bool moved = moves != g_mouseMoves, clicked = clicks != g_mouseClicks;
            g_mouseMoves = moves, g_mouseClicks = clicks;
            input::pc::TakeMenuClick();
            input::pc::TakeMenuWheel();
            float x, y;
            HitZone zone{};
            bool onZone = false;
            if ((moved || clicked) && input::pc::CursorOnImage(x, y))
            {
                std::lock_guard lock(g_mutex);
                onZone = ZoneAt(g_zones, x, y, zone);
            }
            if (onZone && zone.zone == Zone::Cell)
                Select(zone.index);

            if ((pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) || (clicked && onZone && zone.zone == Zone::Back) ||
                MapKeyPressed(node, now))
            {
                Queue(Action::Back);
                Close(clicked ? "click on back" : "B");
            }
            Publish();
        }
    }

    namespace detail
    {
        const Data& GetData()
        {
            if (g_previewData)
                return *g_previewData;
            static const Data data = LoadData(settings::UiLanguageOf(settings::StartupLanguage().id));
            return data;
        }

        void SetPreviewLanguage(const settings::UiLanguage* language)
        {
            g_previewData = language ? std::make_unique<Data>(LoadData(*language)) : nullptr;
        }

        std::map<uint16_t, std::string> Unlocked(uint64_t* version)
        {
            std::lock_guard lock(g_mutex);
            LoadUnlockedLocked();
            if (version)
                *version = g_unlockedVersion;
            return g_unlocked;
        }

        bool TakePopup(uint16_t& id)
        {
            std::lock_guard lock(g_mutex);
            if (g_popups.empty())
                return false;
            id = g_popups.front();
            g_popups.pop_front();
            g_actions.push_back(Action::Unlock);  // its sound, as it appears
            return true;
        }

        Visual CurrentVisual()
        {
            std::lock_guard lock(g_mutex);
            return g_visual;
        }

        bool HintWanted()
        {
            return g_hintWanted && !g_open && !g_releaseHold;
        }

        void SetZones(Zone kind, const std::vector<HitZone>& zones)
        {
            std::lock_guard lock(g_mutex);
            (kind == Zone::Hint ? g_hintZones : g_zones) = zones;
        }
    }

    bool Unlock(uint32_t id, const char* how)
    {
        const Data& data = GetData();
        auto it = std::find_if(data.list.begin(), data.list.end(), [&](const Achievement& a) { return a.id == id; });
        if (it == data.list.end())
        {
            Log("[achievements] unknown achievement %u (%s)", id, how);
            return false;
        }
        {
            std::lock_guard lock(g_mutex);
            LoadUnlockedLocked();
            if (g_unlocked.count(uint16_t(id)))
                return false;  // the game writes an earned one again now and then: XAM ignores it too
            g_unlocked[uint16_t(id)] = Now();
            g_unlockedVersion++;
            g_popups.push_back(uint16_t(id));
            SaveUnlockedLocked();
        }
        Log("[achievements] unlocked %u \"%s\" (%u G, %s)", id, it->name.c_str(), it->gamerscore, how);
        boot::Event("achievement_unlocked", it->name.c_str());
        return true;
    }

    void WriteMessage(uint32_t buffer)
    {
        if (!buffer)
            return;
        uint32_t count = LoadBE32(buffer), list = LoadBE32(buffer + 4);
        for (uint32_t i = 0; i < count && i < 64 && list; i++)
        {
            uint32_t user = LoadBE32(list + i * 8), id = LoadBE32(list + i * 8 + 4);
            Log("[achievements] XUserWriteAchievements: user %u, achievement %u", user, id);
            Unlock(id, "game");
        }
    }

    bool AwardThroughGame(PPCContext& ctx, uint8_t* base, uint32_t id)
    {
        static uint32_t buffer = GuestAlloc(32);
        if (!buffer)
            return false;
        // XUSER_ACHIEVEMENT { user index, achievement id }, as the game builds it on its stack.
        StoreBE32(buffer, 0);
        StoreBE32(buffer + 4, id);
        PPCContext call = ctx;
        sub_821FB4F0(call, base);
        call.r4.s64 = 10;
        sub_821FA440(call, base);
        if (!call.r3.u32)
            return false;
        call.r4.u64 = buffer;
        call.r5.u64 = 1;
        sub_8220A708(call, base);
        Log("[achievements] achievement %u queued through the game's objective manager", id);
        return true;
    }

    std::string List()
    {
        const Data& data = GetData();
        std::map<uint16_t, std::string> unlocked = Unlocked();
        std::string out;
        for (const Achievement& a : data.list)
        {
            auto it = unlocked.find(a.id);
            out += std::to_string(a.id) + " " + a.name + " (" + std::to_string(a.gamerscore) + " G)" +
                   (it != unlocked.end() ? " - " + it->second : "") + "\n";
        }
        return out;
    }

    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll)
    {
        if (poll)
        {
            uint64_t now = plat::TickCountMs();
            uint16_t buttons = pad.wButtons;
            constexpr int16_t STICK = 16000;
            if (pad.sThumbLY > STICK)
                buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (pad.sThumbLY < -STICK)
                buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (pad.sThumbLX > STICK)
                buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (pad.sThumbLX < -STICK)
                buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            uint16_t pressed = buttons & ~g_lastButtons;
            g_lastButtons = buttons;
            if (g_open)
                Drive(buttons, pressed, now);
            else if (g_releaseHold)
            {
                g_hintWanted = false;
                MapKeyPressed(settings::menu::FrontendNode(), now);
                if (!buttons && !g_keyHeld && now - g_closedAt > RELEASE_MS)
                    g_releaseHold = false;
            }
            else
                WatchOpen(pressed, now);
        }
        if (BlocksGameInput())
            pad = {};
        else if (OpenNode(settings::menu::FrontendNode()) && GetData().ok)
            pad.wButtons &= ~XINPUT_GAMEPAD_BACK;  // BACK is the achievements' on these screens
    }

    bool BlocksGameInput()
    {
        return g_open || g_releaseHold;
    }

    bool IsOpen()
    {
        return g_open;
    }

    void RunPending(PPCContext& ctx, uint8_t* base)
    {
        std::deque<Action> actions;
        {
            std::lock_guard lock(g_mutex);
            actions.swap(g_actions);
        }
        for (Action action : actions)
        {
            PPCContext call = ctx;
            sub_821FB4F0(call, base);
            call.r4.s64 = 6;
            sub_821FA440(call, base);
            if (!call.r3.u32)
                continue;
            switch (action)
            {
            case Action::Next: sub_821DB280(call, base); break;
            case Action::Previous: sub_821DB260(call, base); break;
            case Action::Open:
            case Action::Back:
            case Action::Unlock:
            {
                constexpr uint32_t BANK_FE = 0x82006DA4;  // "FE_SFX"
                constexpr uint32_t SOUND_BACK = 0x82006DAC, SOUND_SELECT = 0x82006DB4;  // "back", "select"
                constexpr uint32_t VOLUME = 0x82000DC8;   // the float the frontend's sounds play at
                uint32_t bits = LoadBE32(VOLUME);
                float volume;
                memcpy(&volume, &bits, 4);
                call.r4.u64 = BANK_FE;
                call.r5.u64 = action == Action::Back ? SOUND_BACK : SOUND_SELECT;
                call.f1.f64 = volume;
                sub_821DB1D8(call, base);
                break;
            }
            }
        }
    }
}
