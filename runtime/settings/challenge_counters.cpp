// The player's progress in the challenges that unlock Crash's skins (challenge_counters.h):
//
//   - the HUB's locked skin dolls say "DEFEAT 200 RATNICIANS" / "JACK 50 SPIKES" / "COLLECT 20
//     GOLD IDOLS"; here they say "DEFEAT 37/200 RATNICIANS", "JACK 12/50 SPIKES", "COLLECT 3/20
//     GOLD IDOLS" and "COMPLETE 23%/50% OF GAME", in every language (all of them write the number
//     in digits);
//   - in a level, each time one of those enemies is defeated / jacked, the same text with the new
//     count shows under the game's own minion counter (right side) for a few seconds.
//
// Where the counts are (runtime/NOTES.md "Contador nos desafios das skins"):
//   - the unlocks are objectives of script\bootup_files.blua (the copy the game runs; the loose
//     script\objectives\objectives.blua / .lua are older, with other numbers): UnlockXSkin =
//     WAIT_StatIntValue(EStatClass_Global, EStatInt_..., n, GREATER_THAN_EQUALS) + DO_UnlockSkin;
//     Valentine = WAIT_StatGoldMedals(20), Halloween = WAIT_StatGameCompletionPercent(50).
//   - CStatsManager = manager 16 (0x824A7A24 + (16 + 7) * 4). Int stat s of class c (0 Current,
//     1 Global) = the word at +((c * 93 + s + 114) * 4) (CRequirementStatIntValue::vf01,
//     sub_822194F0); EStatInt values from the enum registration (ppc_recomp.35, sub_822dc9a0
//     calls): 50 Chiratta (Ratnician) ... 55 BratGirl deaths, 30 Spike ... 41 Scorporilla jacked.
//     Gold idols = sub_82248228(stats), completion % = sub_822485E0(stats) (what the requirements
//     CRequirementStatGoldMedals / ...GameCompletionPercent compare).
//
// Where the text is: a frontend text item (+112 its text keys, +148 the text bible, +156 the
// UTF-16 string it shows; +152 = a string set by code, which stops the key lookup) gets its
// string from sub_82315950(bible, index) = the current language's buffer + offset
// (sub_822F2140; language = bible+20 [bible+36], its keys at +24, sorted). That function is hooked:
// for a challenge's key, whose string still holds the challenge's number, it returns a copy with
// the count in front of the number. The copy is a guest buffer per challenge, rewritten at each
// lookup (the item keeps the pointer until its next lookup; the counts only change in a level).
// The bible keys are those of the frontend bible (packages 7efdcd91 / cdd70a8c, the same in every
// language and in the PT-BR pack); the number check keeps a key of another bible from matching.
//
// In a level: the frontend bible isn't looked up there, so the first time the hook sees a language
// of it (any lookup at the main menu, which every boot goes through) the challenges' texts are
// copied to the host. Draw (render thread) reads the 17 enemy stats while the frontend node is 638
// (GameRunning): a stat that went up by 1-5 since the last frame (a bigger jump is a save or a
// checkpoint, only resynced) and was below its target shows its line. One line per enemy: the same
// enemy again updates its line and restarts its time; another enemy gets the next line down; when
// a line ends the lines under it slide up, so they never overlap. The game's own counter
// (CFodderCounter, DO_DisplayFodderCounter in each level's objectives: "1 / 65" with the minion's
// icon, right side, y ~185-310 of 720) is left alone; the lines start under it.

#include "challenge_counters.h"
#include "menu_art.h"
#include "settings_menu.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82315950);  // text bible: the string at index r4 of the current language
PPC_EXTERN_FUNC(sub_82248228);             // CStatsManager: gold idols
PPC_EXTERN_FUNC(sub_822485E0);             // CStatsManager: game completion percent

namespace
{
    constexpr uint32_t STATS_MANAGER = 0x824A7A24 + (16 + 7) * 4;
    constexpr uint32_t STAT_CLASS_GLOBAL = 1;
    constexpr uint32_t STATS_PER_CLASS = 93;
    constexpr uint32_t BUFFER_CHARS = 160;
    constexpr uint32_t NODE_GAME_RUNNING = 638;

    enum class Kind { Stat, GoldIdols, Percent };

    struct Challenge
    {
        uint32_t key;       // frontend bible key
        Kind kind;
        uint32_t stat;      // EStatInt (Kind::Stat)
        uint32_t target;    // the number in the text (and in bootup_files)
        const char* name;   // for the log
        uint32_t buffer;    // guest copy of the text with the count
        int32_t logged;     // last count logged
    };

    Challenge g_challenges[] = {
        { 0xF9DD, Kind::Stat, 50, 200, "Ratnicians defeated (Chiratta)" },
        { 0x7620, Kind::Stat, 52, 150, "Koo-alas defeated (Koala)" },
        { 0x7B68, Kind::Stat, 53, 300, "Doom Monkeys defeated" },
        { 0x30CE, Kind::Stat, 54, 200, "Voodoo Bunnies defeated (Rabbit)" },
        { 0xF927, Kind::Stat, 55, 150, "Brat Girls defeated" },
        { 0x6B5E, Kind::Stat, 30, 50, "Spikes jacked" },
        { 0x46E8, Kind::Stat, 31, 50, "Magmadons jacked (Shurtle)" },
        { 0x9199, Kind::Stat, 32, 50, "Sludges jacked" },
        { 0xAF07, Kind::Stat, 33, 50, "Ratcicles jacked" },
        { 0xA841, Kind::Stat, 34, 35, "Battlers jacked" },
        { 0x2EAE, Kind::Stat, 35, 30, "Goars jacked (Grizzly)" },
        { 0x7FBB, Kind::Stat, 36, 30, "Rhinorollers jacked (Roller)" },
        { 0x7B14, Kind::Stat, 37, 50, "Snipes jacked (Parafox)" },
        { 0x6D01, Kind::Stat, 38, 50, "Stenches jacked (Stinky)" },
        { 0x7562, Kind::Stat, 39, 50, "Ee-lectrics jacked" },
        { 0x1D3B, Kind::Stat, 40, 15, "Shellephants jacked" },
        { 0x3B77, Kind::Stat, 41, 15, "Scorporillas jacked" },
        { 0xD2EB, Kind::GoldIdols, 0, 20, "gold idols" },
        { 0x2594, Kind::Percent, 0, 50, "game completion %" },
    };
    constexpr size_t CHALLENGES = sizeof(g_challenges) / sizeof(g_challenges[0]);

    // The challenges' texts as the frontend bible has them, for the level (lock: g_textsMutex).
    std::mutex g_textsMutex;
    std::u16string g_texts[CHALLENGES];
    bool g_haveTexts = false;
    uint32_t g_checkedLanguages[16] = {};

    Challenge* Find(uint32_t key)
    {
        for (Challenge& c : g_challenges)
            if (c.key == key)
                return &c;
        return nullptr;
    }

    bool Plausible(uint32_t pointer)
    {
        return (pointer >= 0x40000000 && pointer < 0x7F000000) || (pointer >= 0x82000000 && pointer < 0x90000000) ||
               pointer >= 0xA0000000;
    }

    uint16_t LoadBE16(uint32_t guest)
    {
        return __builtin_bswap16(*reinterpret_cast<uint16_t*>(g_memory.Translate(guest)));
    }

    uint32_t StatsManager()
    {
        uint32_t stats = LoadBE32(STATS_MANAGER);
        return Plausible(stats) ? stats : 0;
    }

    int32_t GlobalStat(uint32_t stats, uint32_t stat)
    {
        return int32_t(LoadBE32(stats + (STAT_CLASS_GLOBAL * STATS_PER_CLASS + stat + 114) * 4));
    }

    // The player's count for a challenge, or -1 when the stats manager isn't there.
    int32_t Count(const Challenge& c, PPCContext& ctx, uint8_t* base)
    {
        uint32_t stats = StatsManager();
        if (!stats)
            return -1;
        if (c.kind == Kind::Stat)
            return GlobalStat(stats, c.stat);
        PPCContext call = ctx;
        call.r3.u64 = stats;
        if (c.kind == Kind::GoldIdols)
            sub_82248228(call, base);
        else
            sub_822485E0(call, base);
        return call.r3.s32;
    }

    // Where the challenge's number is in `text`, if it is the challenge's number.
    bool FindNumber(const std::u16string& text, const Challenge& c, size_t& first, size_t& last)
    {
        first = 0;
        while (first < text.size() && (text[first] < u'0' || text[first] > u'9'))
            first++;
        last = first;
        uint32_t number = 0;
        while (last < text.size() && text[last] >= u'0' && text[last] <= u'9' && number < 100000)
            number = number * 10 + (text[last++] - u'0');
        return first < text.size() && number == c.target && text.find(u'/') == std::u16string::npos;
    }

    // The text with the count in front of its first number, if that number is the challenge's.
    bool Rewrite(const std::u16string& text, const Challenge& c, int32_t count, std::u16string& out)
    {
        size_t first, last;
        if (!FindNumber(text, c, first, last))
            return false;
        count = std::clamp(count, 0, int32_t(c.target));
        std::string digits = std::to_string(count) + (c.kind == Kind::Percent ? "%/" : "/");
        out = text.substr(0, first);
        for (char d : digits)
            out += char16_t(d);
        out += text.substr(first);
        return out.size() < BUFFER_CHARS;
    }

    std::u16string ReadString(uint32_t string)
    {
        std::u16string text;
        for (uint32_t p = string; text.size() < BUFFER_CHARS; p += 2)
        {
            char16_t ch = char16_t(LoadBE16(p));
            if (!ch)
                break;
            text += ch;
        }
        return text;
    }

    // The string of `key` in a language (keys sorted: binary search, as sub_822F2360), or 0.
    uint32_t LanguageString(uint32_t language, uint32_t key)
    {
        uint32_t count = LoadBE32(language + 20), keys = LoadBE32(language + 24), offsets = LoadBE32(language + 28),
                 buffer = LoadBE32(language + 32);
        if (!count || count > 0x10000 || !Plausible(keys) || !Plausible(offsets) || !Plausible(buffer))
            return 0;
        uint32_t lo = 0, hi = count;
        while (lo < hi)
        {
            uint32_t mid = (lo + hi) / 2, k = LoadBE32(keys + mid * 4);
            if (k == key)
                return buffer + (LoadBE32(offsets + mid * 4) & ~1u);
            if (k < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return 0;
    }

    // A language seen for the first time: if it is the frontend bible's (its Ratnician challenge
    // says 200), keep the challenges' texts for the level.
    void NoteLanguage(uint32_t language)
    {
        for (uint32_t& checked : g_checkedLanguages)
        {
            if (checked == language)
                return;
            if (!checked)
            {
                checked = language;
                break;
            }
        }
        uint32_t probe = LanguageString(language, g_challenges[0].key);
        size_t first, last;
        if (!probe || !FindNumber(ReadString(probe), g_challenges[0], first, last))
            return;
        std::lock_guard<std::mutex> lock(g_textsMutex);
        int kept = 0;
        for (size_t i = 0; i < CHALLENGES; i++)
        {
            uint32_t string = LanguageString(language, g_challenges[i].key);
            g_texts[i] = string ? ReadString(string) : std::u16string();
            kept += !g_texts[i].empty();
        }
        if (!g_haveTexts)
            Log("[challenges] the frontend bible's challenge texts kept for the levels (%d of %zu)", kept, CHALLENGES);
        g_haveTexts = true;
    }
}

PPC_FUNC(sub_82315950)
{
    PPCContext call = ctx;
    uint32_t bible = ctx.r3.u32;
    int32_t index = ctx.r4.s32;
    __imp__sub_82315950(ctx, base);
    uint32_t string = ctx.r3.u32;
    if (!string || index < 0 || !Plausible(bible))
        return;

    uint32_t languages = LoadBE32(bible + 20);
    int16_t current = int16_t(LoadBE16(bible + 36));
    if (!Plausible(languages) || current < 0)
        return;
    uint32_t language = LoadBE32(languages + uint32_t(current) * 4);
    if (!Plausible(language) || uint32_t(index) >= LoadBE32(language + 20))
        return;
    NoteLanguage(language);
    Challenge* c = Find(LoadBE32(LoadBE32(language + 24) + uint32_t(index) * 4));
    if (!c)
        return;

    int32_t count = Count(*c, call, base);
    std::u16string shown;
    if (count < 0 || !Rewrite(ReadString(string), *c, count, shown))
        return;

    if (!c->buffer)
        c->buffer = GuestAlloc(BUFFER_CHARS * 2);
    if (!c->buffer)
        return;
    for (size_t i = 0; i < shown.size(); i++)
        StoreBE16(c->buffer + uint32_t(i) * 2, uint16_t(shown[i]));
    StoreBE16(c->buffer + uint32_t(shown.size()) * 2, 0);
    ctx.r3.u64 = c->buffer;

    if (c->logged != count + 1)
    {
        c->logged = count + 1;
        Log("[challenges] key 0x%04X: %s %d/%u", c->key, c->name, count, c->target);
    }
}

namespace settings::challenges
{
    namespace
    {
        using menu_art::Color;
        using menu_art::FontId;

        // A line comes in from beyond the right edge of the screen and, SHOWN_MS after its last
        // count, goes back out the same way (like the game's counter): SLIDE_MS each way, eased
        // at both ends (the user found ~0.3 s too fast).
        constexpr uint64_t SHOWN_MS = 3000;
        constexpr float SLIDE_MS = 700.0f;
        constexpr size_t MAX_LINES = 5;

        struct Line
        {
            size_t challenge;
            std::u32string before, number, after;  // the text around "count/target"
            bool done;                             // count reached the target
            uint64_t updated;                      // last count
            float y = -1.0f;                       // current row position (slides to its row)
            float out = 1.0f;                      // slide: 0 = in place, 1 = just beyond the right edge (linear; eased when drawn)
        };

        std::vector<Line> g_lines;
        int32_t g_previous[CHALLENGES];
        bool g_synced = false;

        std::u32string Utf32(const std::u16string& text)
        {
            return std::u32string(text.begin(), text.end());
        }

        // A line for challenge i at `count` (false without the text).
        bool Compose(size_t i, int32_t count, Line& line)
        {
            std::u16string text;
            {
                std::lock_guard<std::mutex> lock(g_textsMutex);
                text = g_texts[i];
            }
            size_t first, last;
            if (text.empty() || !FindNumber(text, g_challenges[i], first, last))
                return false;
            count = std::clamp(count, 0, int32_t(g_challenges[i].target));
            line.challenge = i;
            // Without the verb before the number (DEFEAT / JACK, DERROTE / DOMINE, DERROTA A...) and
            // with SKIN before the enemy (the user's wording, the same in every language):
            // "2/200 SKIN RATNICIANS".
            line.before.clear();
            line.number = menu_art::Utf32(std::to_string(count) + "/") + Utf32(text.substr(first, last - first));
            line.after = U" SKIN" + Utf32(text.substr(last));
            line.done = uint32_t(count) >= g_challenges[i].target;
            return true;
        }

        // New counts of the enemy challenges (stats that went up by 1-5 since the last call).
        void Poll(uint64_t now)
        {
            uint32_t stats = StatsManager();
            bool playing = stats && menu::FrontendNode() == NODE_GAME_RUNNING;
            if (!playing)
            {
                g_synced = false;
                return;
            }
            for (size_t i = 0; i < CHALLENGES; i++)
            {
                const Challenge& c = g_challenges[i];
                if (c.kind != Kind::Stat)
                    continue;
                int32_t value = GlobalStat(stats, c.stat);
                int32_t before = g_previous[i];
                g_previous[i] = value;
                if (!g_synced || value <= before || value - before > 5 || before >= int32_t(c.target))
                    continue;
                Line line;
                if (!Compose(i, value, line))
                    continue;
                auto same = std::find_if(g_lines.begin(), g_lines.end(), [&](const Line& l) { return l.challenge == i; });
                if (same != g_lines.end())
                {
                    line.y = same->y, line.out = same->out;
                    *same = line;
                    same->updated = now;
                }
                else
                {
                    if (g_lines.size() >= MAX_LINES)
                        g_lines.erase(g_lines.begin());
                    line.updated = now;
                    g_lines.push_back(line);
                }
                Log("[challenges] in level: %s %d/%u", c.name, value, c.target);
            }
            g_synced = true;
        }
    }

    void Draw(uint32_t width, uint32_t height)
    {
        static uint64_t last = 0;
        uint64_t now = plat::TickCountMs();
        float dt = float(std::min<uint64_t>(now - last, 100));
        last = now;
        Poll(now);
        // In or out: each line's slide moves toward its goal (out once its time is over); a line
        // that is all the way out is gone.
        float step = dt / SLIDE_MS;
        for (Line& l : g_lines)
        {
            float goal = now - l.updated > SHOWN_MS ? 1.0f : 0.0f;
            l.out = goal > l.out ? std::min(goal, l.out + step) : std::max(goal, l.out - step);
        }
        g_lines.erase(std::remove_if(g_lines.begin(), g_lines.end(),
                                     [&](const Line& l) { return now - l.updated > SHOWN_MS && l.out >= 1.0f; }),
                      g_lines.end());
        if (g_lines.empty() || !g_synced || !width || !height || !menu_art::Ready(0))
            return;

        const float s = height / 720.0f;
        const float cell = 28.0f * s, row = 36.0f * s, margin = 34.0f * s, top = 326.0f * s;
        const menu_art::TextStyle text{ { 255, 255, 255, 255 }, { 28, 14, 4, 255 } };
        const menu_art::TextStyle number{ { 255, 214, 74, 255 }, { 28, 14, 4, 255 } };
        const menu_art::TextStyle done{ { 136, 236, 96, 255 }, { 28, 14, 4, 255 } };

        // Rows: each line slides to its place (a line that ended lets the ones under it move up).
        float maxW = 0.0f;
        for (size_t i = 0; i < g_lines.size(); i++)
        {
            Line& l = g_lines[i];
            float target = i * row;
            l.y = l.y < 0.0f ? target : l.y + (target - l.y) * 0.25f;
            if (std::fabs(l.y - target) < 0.5f)
                l.y = target;
            float w = menu_art::TextWidth(FontId::Large, l.before + l.number + l.after, cell);
            maxW = std::max(maxW, w);
        }
        // The canvas ends at the screen's right edge: a line beyond it is cut there, as off screen.
        int cw = int(std::ceil(maxW + margin)), ch = int(std::ceil(g_lines.size() * row + cell));
        cw = std::min(cw, int(width)), ch = std::min(ch, int(height));

        static menu_art::Canvas canvas;
        static std::vector<uint32_t> pixels;
        static uint64_t version = 0;
        canvas.Reset(cw, ch);
        for (const Line& l : g_lines)
        {
            float w = menu_art::TextWidth(FontId::Large, l.before + l.number + l.after, cell);
            float eased = l.out * l.out * (3.0f - 2.0f * l.out);  // smoothstep: slow start and end
            float x = cw - margin - w + eased * (w + margin + 4.0f * s);
            const menu_art::TextStyle& a = text;
            const menu_art::TextStyle& b = l.done ? done : number;
            x = menu_art::DrawGameText(canvas, FontId::Large, l.before, x, l.y, cell, a);
            x = menu_art::DrawGameText(canvas, FontId::Large, l.number, x, l.y, cell, b);
            menu_art::DrawGameText(canvas, FontId::Large, l.after, x, l.y, cell, a);
        }
        canvas.Export(pixels);
        gpu::overlay::DrawBitmap(width, height, pixels.data(), cw, ch, int(width) - cw, int(std::lround(top)), ++version,
                                 gpu::overlay::BITMAP_CHALLENGES);
    }
}
