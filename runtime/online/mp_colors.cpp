// The players' Crash colours (mp_colors.h; docs/internals/multiplayer.md, "Colours").

#include "mp_colors.h"
#include "mp.h"
#include "mp_lobby.h"
#include "mp_menu.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_822212C0);  // character drawable: the palette of this frame (+0x11C8)
PPC_EXTERN_FUNC(sub_8222C2D8);             // character drawable (+348): sets its base palette

namespace
{
    using mp::colors::Colour;

    // How a colour is made: the normal palette as it is, the jacked one, or the normal one with its fur
    // recoloured. The fur's hue goes to `hue` (its spread around the orange's 25 degrees times
    // `spread`), its saturation times `sat`, and its value through vMin + (vMax - vMin) * v^gamma: the
    // shading and the shine of the fur stay, darker or lighter (black and white can't come from the hue).
    enum class Kind { Normal, Jacked, Tint };
    struct Recipe
    {
        Colour names;
        Kind kind;
        float hue = 0, spread = 1, sat = 1, vMin = 0, vMax = 1, gamma = 1;
    };

    // Like the game's promotional art with many coloured Crashes (a crowd of them at the village):
    // coral red, salmon pink, violet, indigo blue, leaf green, golden yellow, nothing neon, the fur's
    // shading kept. In the order the menus go through them.
    const Recipe RECIPES[] = {
        { { "orange", "ORANGE", "LARANJA" }, Kind::Normal },
        { { "red", "RED", "VERMELHO" }, Kind::Tint, 4, 0.4f, 0.9f, 0.0f, 0.92f, 1.05f },
        { { "pink", "PINK", "ROSA" }, Kind::Tint, 350, 0.35f, 0.56f, 0.12f, 1.0f, 0.95f },
        { { "purple", "PURPLE", "ROXO" }, Kind::Tint, 276, 0.4f, 0.62f, 0.02f, 0.82f, 1.0f },
        { { "blue", "BLUE", "AZUL" }, Kind::Tint, 240, 0.4f, 0.62f, 0.02f, 0.86f, 1.0f },
        { { "cyan", "CYAN", "CIANO" }, Kind::Tint, 186, 0.4f, 0.6f, 0.04f, 0.9f, 1.0f },
        { { "green", "GREEN", "VERDE" }, Kind::Tint, 118, 0.45f, 0.7f, 0.0f, 0.82f, 1.0f },
        { { "yellow", "YELLOW", "AMARELO" }, Kind::Tint, 47, 0.3f, 0.84f, 0.10f, 1.0f, 0.9f },
        { { "brown", "BROWN", "MARROM" }, Kind::Tint, 24, 0.3f, 0.72f, 0.02f, 0.56f, 1.1f },
        { { "white", "WHITE", "BRANCO" }, Kind::Jacked },
        { { "gray", "GRAY", "CINZA" }, Kind::Tint, 30, 0.0f, 0.08f, 0.10f, 0.74f, 1.0f },
        { { "black", "BLACK", "PRETO" }, Kind::Tint, 228, 0.0f, 0.22f, 0.03f, 0.30f, 1.15f },
    };
    constexpr uint32_t COUNT = sizeof(RECIPES) / sizeof(RECIPES[0]);
    static_assert(COUNT >= mp::colors::MAX_SLOTS, "every slot needs a colour of its own");

    // Without a lobby: slot 1 orange, 2 white (the game's own two), then blue, green, red, purple, yellow, pink.
    const char* const DEFAULTS[mp::colors::MAX_SLOTS] = { "orange", "white", "blue", "green", "red", "purple", "yellow", "pink" };

    // The orange fur's mid tone (the sample of the normal palette in the menus).
    constexpr uint8_t REFERENCE[3] = { 226, 116, 30 };
    constexpr uint8_t PALE[3] = { 236, 228, 212 };  // the jacked palette's fur

    bool GuestPointer(uint32_t p) { return (p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000); }

    void ToHsv(float r, float g, float b, float& h, float& s, float& v)
    {
        float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), delta = mx - mn;
        v = mx;
        s = mx > 0 ? delta / mx : 0;
        h = 0;
        if (delta > 0)
        {
            h = mx == r ? 60.0f * std::fmod((g - b) / delta, 6.0f) : mx == g ? 60.0f * ((b - r) / delta + 2) : 60.0f * ((r - g) / delta + 4);
            if (h < 0)
                h += 360;
        }
    }

    void ToRgb(float h, float s, float v, float rgb[3])
    {
        float nh = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
        float c = v * s, x = c * (1 - std::abs(std::fmod(nh, 2.0f) - 1)), m = v - c;
        switch (int(nh) % 6)
        {
        case 0: rgb[0] = c, rgb[1] = x, rgb[2] = 0; break;
        case 1: rgb[0] = x, rgb[1] = c, rgb[2] = 0; break;
        case 2: rgb[0] = 0, rgb[1] = c, rgb[2] = x; break;
        case 3: rgb[0] = 0, rgb[1] = x, rgb[2] = c; break;
        case 4: rgb[0] = x, rgb[1] = 0, rgb[2] = c; break;
        default: rgb[0] = c, rgb[1] = 0, rgb[2] = x; break;
        }
        for (int k = 0; k < 3; ++k)
            rgb[k] += m;
    }

    // Crash's fur: the saturated oranges of the normal palette, and its darker, duller browns (the
    // back and the neck); the skin (light, less saturated), the jeans and the shoes are outside it.
    bool IsFur(float h, float s, float v) { return v > 0 && h >= 5 && h <= 45 && s > (v < 0.55f ? 0.3f : 0.5f); }

    // One fur colour through a recipe (bytes in, bytes out). False: not the fur (kept).
    bool Recolour(const Recipe& recipe, uint8_t c[3])
    {
        float h, s, v;
        ToHsv(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, h, s, v);
        if (!IsFur(h, s, v))
            return false;
        float rgb[3];
        ToRgb(recipe.hue + (h - 25.0f) * recipe.spread, std::clamp(s * recipe.sat, 0.0f, 1.0f),
              recipe.vMin + (recipe.vMax - recipe.vMin) * std::pow(v, recipe.gamma), rgb);
        for (int k = 0; k < 3; ++k)
            c[k] = uint8_t(std::clamp(rgb[k] * 255.0f + 0.5f, 0.0f, 255.0f));
        return true;
    }

    // Skins (dev_skins.cpp) have palettes of their own (each package's crash_normal / Crash_Player2)
    // and a costume over Crash. tools/mp_skin_colours.py makes the table from the user's extracted game
    // data, at build time into the build folder (game data, never in the repository; without the data
    // it is empty and every palette gets the fur test below): per skin the palette entries
    // of the costume's main colour (the jeans left out), its hue, mean saturation and bright value,
    // and 16 sample entries by which a drawable's palette is recognised. Those entries take the
    // colour like the fur (hue around the costume's own, saturation and value normalised to the
    // fur's), so the whole costume changes, as the skin's own P2 palette (WHITE) changes it; a costume
    // without a colour (Skeleton) gets its light entries coloured.
    struct SkinPalette
    {
        const char* name;
        uint8_t samples[16][3];  // entries 0, 16, ..., 240
        uint8_t mask[32];        // a bit per entry: the costume's colour
        float centre, sMean, v95;
        bool colourize;
    };
    const SkinPalette SKINS[] = {
#include "mp_skin_palettes.inc"
        { nullptr },
    };
    constexpr float FUR_SAT = 0.8f;  // the normal Crash's fur, its mean saturation

    // The skin a drawable's palette (big-endian ARGB words) is, -1 if none (the normal Crash).
    int FindSkin(const uint8_t* words)
    {
        for (int i = 0; SKINS[i].name; ++i)
        {
            bool same = true;
            for (int k = 0; k < 16 && same; ++k)
                for (int ch = 0; ch < 3; ++ch)
                    same = same && std::abs(int(words[k * 64 + 1 + ch]) - int(SKINS[i].samples[k][ch])) <= 4;
            if (same)
                return i;
        }
        return -1;
    }

    void RecolourSkin(const Recipe& recipe, const SkinPalette& skin, uint8_t c[3])
    {
        float h, s, v;
        ToHsv(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, h, s, v);
        float nh = recipe.hue, ns = std::clamp(0.55f * recipe.sat, 0.0f, 1.0f);
        if (!skin.colourize)
        {
            nh = recipe.hue + (std::fmod(h - skin.centre + 540.0f, 360.0f) - 180.0f) * recipe.spread;
            ns = std::clamp(s / skin.sMean * FUR_SAT * recipe.sat, 0.0f, 1.0f);
        }
        float rgb[3];
        ToRgb(nh, ns, recipe.vMin + (recipe.vMax - recipe.vMin) * std::pow(std::min(v / skin.v95, 1.0f), recipe.gamma), rgb);
        for (int k = 0; k < 3; ++k)
            c[k] = uint8_t(std::clamp(rgb[k] * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    // A palette that is neither Crash's nor a known skin's: its fur only when at least this many of
    // its 256 entries are (Crash's has 132), else it keeps its look (specks or patches otherwise).
    constexpr int MIN_FUR_ENTRIES = 64;

    // The final palette of a drawable (256 big-endian ARGB words); skin: FindSkin's.
    void TintPalette(uint32_t palette, const Recipe& recipe, int skin)
    {
        uint8_t* words = g_memory.Translate(palette);
        if (skin >= 0)
        {
            for (int e = 0; e < 256; ++e)
                if (SKINS[skin].mask[e >> 3] & (1 << (e & 7)))
                    RecolourSkin(recipe, SKINS[skin], words + e * 4 + 1);
            return;
        }
        int fur = 0;
        for (int e = 0; e < 256; ++e)
        {
            const uint8_t* c = words + e * 4 + 1;
            float h, s, v;
            ToHsv(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, h, s, v);
            fur += IsFur(h, s, v);
        }
        if (fur < MIN_FUR_ENTRIES)
            return;
        for (int e = 0; e < 256; ++e)
            Recolour(recipe, words + e * 4 + 1);
    }

    std::atomic<int> g_mine{ -1 };
    bool g_mineFromEnv = false;
    std::atomic<bool> g_session{ false };
    std::atomic<uint8_t> g_slots[mp::colors::MAX_SLOTS];

    struct Painted
    {
        uint32_t colour;
        int skin = -2;  // FindSkin's, -2 not looked yet
    };
    // The skin screen's 3D preview while MULTIPLAYER's SKIN AND COLOR shows it (mp_menu.h PreviewColour), by
    // drawable: its normal palette (a new skin's drawable may take an old one's address), whether its base is
    // the jacked palette (WHITE) and its skin (FindSkin, from its first frame). Game thread.
    struct Preview
    {
        uint32_t normal = 0;
        bool jacked = false;
        int skin = -1;
    };
    std::unordered_map<uint32_t, Preview> g_previews;

    // The preview in the player's colour, as a player's Crash: the base is the jacked palette for WHITE and the
    // normal one for the others (so the game's blend gives it), and a Tint recolours this frame's palette.
    void PaintPreview(PPCContext& ctx, uint8_t* base, uint32_t drawable, uint32_t colour);

    std::mutex g_paintMutex;
    std::unordered_map<uint32_t, Painted> g_painted;  // by drawable

    // CRASH_MP_COLORS (tests).
    struct TestList
    {
        std::vector<uint32_t> colours;
        uint64_t every = 0, start = 0;
    };
    const TestList& Tests()
    {
        static TestList list = [] {
            TestList t;
            if (const char* env = getenv("CRASH_MP_COLORS"))
            {
                std::string text = env, item;
                for (size_t i = 0; i <= text.size(); ++i)
                {
                    if (i == text.size() || text[i] == ',')
                    {
                        if (int c = mp::colors::Find(item); c >= 0)
                            t.colours.push_back(uint32_t(c));
                        item.clear();
                    }
                    else
                        item += char(std::tolower(uint8_t(text[i])));
                }
            }
            if (const char* every = getenv("CRASH_MP_COLORS_EVERY"))
                t.every = uint64_t(std::max(0, atoi(every)));
            t.start = mp::NowMs();
            return t;
        }();
        return list;
    }
}

namespace mp::colors
{
    uint32_t Count() { return COUNT; }
    const Colour& Get(uint32_t index) { return RECIPES[std::min(index, COUNT - 1)].names; }

    int Find(const std::string& id)
    {
        for (uint32_t i = 0; i < COUNT; ++i)
            if (id == RECIPES[i].names.id)
                return int(i);
        return -1;
    }

    void Swatch(uint32_t index, uint8_t rgb[3])
    {
        const Recipe& recipe = RECIPES[std::min(index, COUNT - 1)];
        std::copy(recipe.kind == Kind::Jacked ? PALE : REFERENCE, (recipe.kind == Kind::Jacked ? PALE : REFERENCE) + 3, rgb);
        if (recipe.kind == Kind::Tint)
            Recolour(recipe, rgb);
    }

    uint32_t Mine()
    {
        int mine = g_mine.load();
        if (mine < 0)
        {
            if (const char* env = getenv("CRASH_MP_COLOR"))
            {
                mine = Find(env);
                g_mineFromEnv = mine >= 0;
            }
            if (mine < 0)
                mine = Find(settings::Get().mpColour);
            g_mine = mine = std::max(mine, 0);
        }
        return uint32_t(mine);
    }

    uint32_t NextFree(uint32_t from, int direction, uint32_t taken)
    {
        for (uint32_t k = 0; k < COUNT; ++k)
        {
            uint32_t c = (from + COUNT + uint32_t(direction) * k) % COUNT;
            if (!(taken & (1u << c)))
                return c;
        }
        return from % COUNT;
    }

    void Step(int direction)
    {
        direction = direction < 0 ? -1 : 1;
        uint32_t current = Mine(), taken = 0;
        lobby::State state = lobby::GetState();
        if (state == lobby::State::Hosting || state == lobby::State::Joined)
            for (const lobby::Player& p : lobby::Players())
            {
                if (p.me && p.colour < COUNT)
                    current = p.colour;  // what the host gave this player (maybe not what it asked for)
                else if (!p.me && p.colour < COUNT)
                    taken |= 1u << p.colour;
            }
        uint32_t next = NextFree((current + COUNT + uint32_t(direction)) % COUNT, direction, taken);
        g_mine = int(next);
        Log("[mp] my colour: %s", RECIPES[next].names.id);
        if (g_mineFromEnv)
            return;
        settings::Values values = settings::Get();
        values.mpColour = RECIPES[next].names.id;
        settings::Set(values);
        settings::Save();
    }

    void SetSlotColours(const uint8_t* colours, uint32_t count)
    {
        for (uint32_t i = 0; i < MAX_SLOTS; ++i)
            g_slots[i] = i < count ? colours[i] : NONE;
        g_session = true;
    }

    void ClearSlotColours() { g_session = false; }

    uint32_t ForSlot(uint32_t slot)
    {
        if (slot >= MAX_SLOTS)
            return 0;
        if (g_session)
            if (uint8_t c = g_slots[slot]; c < COUNT)
                return c;
        const TestList& tests = Tests();
        if (!tests.colours.empty())
        {
            uint64_t step = tests.every ? (NowMs() - tests.start) / tests.every : 0;
            return tests.colours[(slot + step * std::max<uint32_t>(1, mp::Players())) % tests.colours.size()];
        }
        return uint32_t(std::max(0, Find(DEFAULTS[slot])));
    }

    bool UsesJacked(uint32_t colour) { return RECIPES[std::min(colour, COUNT - 1)].kind == Kind::Jacked; }

    void Paint(uint32_t drawable, uint32_t colour)
    {
        std::lock_guard<std::mutex> lock(g_paintMutex);
        g_painted[drawable] = { std::min(colour, COUNT - 1) };
    }

    int PaintedWith(uint32_t drawable)
    {
        std::lock_guard<std::mutex> lock(g_paintMutex);
        auto it = g_painted.find(drawable);
        return it == g_painted.end() ? -1 : int(it->second.colour);
    }

    void Forget(uint32_t drawable)
    {
        std::lock_guard<std::mutex> lock(g_paintMutex);
        g_painted.erase(drawable);
    }
}

// After the game blends a character's palette for the frame: a player's fur (or skin's costume) gets
// its colour.
PPC_FUNC(sub_822212C0)
{
    uint32_t drawable = ctx.r3.u32;
    __imp__sub_822212C0(ctx, base);
    if (int colour = mp::menu::PreviewColour(); colour >= 0)
    {
        PaintPreview(ctx, base, drawable, uint32_t(colour));
        return;
    }
    if (!g_previews.empty())
        g_previews.clear();
    if (!mp::Enabled())
        return;
    Painted painted{ 0, -2 };
    {
        std::lock_guard<std::mutex> lock(g_paintMutex);
        auto it = g_painted.find(drawable);
        if (it == g_painted.end())
            return;
        painted = it->second;
    }
    if (RECIPES[painted.colour].kind != Kind::Tint)
        return;
    // Still a player's (an old level's drawable may have been freed and its memory reused).
    for (uint32_t i = 0; i < mp::colors::MAX_SLOTS; ++i)
        if (uint32_t crash = mp::PlayerCrash(i))
            if (uint32_t render = LoadBE32(crash + 0xE0); GuestPointer(render) && LoadBE32(render + 92) == drawable)
            {
                if (painted.skin == -2)
                {
                    // Before this frame's recolour: the palette as the game blended it (base = target).
                    painted.skin = FindSkin(g_memory.Translate(drawable + 0x11C8));
                    Log("[mp] drawable %08X: %s palette", drawable, painted.skin >= 0 ? SKINS[painted.skin].name : "Crash's (or unknown)");
                    std::lock_guard<std::mutex> lock(g_paintMutex);
                    if (auto it = g_painted.find(drawable); it != g_painted.end())
                        it->second.skin = painted.skin;
                }
                TintPalette(drawable + 0x11C8, RECIPES[painted.colour], painted.skin);
                return;
            }
}

namespace
{
    void PaintPreview(PPCContext& ctx, uint8_t* base, uint32_t drawable, uint32_t colour)
    {
        const Recipe& recipe = RECIPES[std::min(colour, COUNT - 1)];
        uint32_t variant = LoadBE32(drawable + 4548) & 3;
        uint32_t normal = LoadBE32(drawable + 4516 + variant * 4), jacked = LoadBE32(drawable + 4532 + variant * 4);
        if (!GuestPointer(normal) || !GuestPointer(jacked))
            return;
        Preview& p = g_previews[drawable];
        if (p.normal != normal)
        {
            p = { normal, false, FindSkin(g_memory.Translate(drawable + 0x11C8)) };
            Log("[mp] skin screen preview %08X: %s palette", drawable, p.skin >= 0 ? SKINS[p.skin].name : "Crash's (or unknown)");
        }
        bool wantJacked = recipe.kind == Kind::Jacked;
        if (p.jacked != wantJacked)
        {
            PPCContext call = ctx;
            call.r3.u64 = drawable + 348;
            call.r4.u64 = (wantJacked ? jacked : normal) + 16;
            sub_8222C2D8(call, base);
            p.jacked = wantJacked;
        }
        if (recipe.kind == Kind::Tint)
            TintPalette(drawable + 0x11C8, recipe, p.skin);
    }
}
