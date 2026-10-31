// Crash's skins: the console's "skin" / "unlock skin" commands and the unused Crash_Hulk skin
// (dev_tools.h; docs/dev_features.md "Skins"; README "Console de desenvolvedor").
//
// How the game applies a skin (the HUB's voodoo dolls -> SkinSelectionScreen -> "wear"):
//
//   ESkin -> name           sub_82215548 (table: 0 Chiratta ... 17 Scorporilla, 21 Crash, 22 Crash_Hulk,
//                           23 Valentine, 24 Skeleton = ESkin_Halloween; 18-20 have none: null)
//   ESkin -> template hash  sub_82215730: hash("Characters:" + ("Crash_" unless 21) + name), the
//                           game's name hash (sub_822846C0, h = h * 31 + c): the template actor
//                           "Characters:Crash_Ratcicle" etc. of levels\GlobalPackages.p3d
//   current skin            0x824A8270, the template hash (getter sub_822159E8, setter sub_822159C0;
//                           "Characters:Crash" at boot, sub_82421420); not in the save
//   Frontend:SelectSkin     sub_820F6180 -> sub_821D9270(frontend manager, skin): sets the current
//                           skin, loads the new template's package (CStreamManager sub_8224D3C8) and
//                           releases the old one (sub_8224CEC8)
//   Frontend:SetSkinToDisplay / CheckSkinToView: the skin screen's skin (sub_820BBDB8 = *0x82497508)
//   level load / unload     CStreamManager::vf05 (sub_8224E068) loads the player 2 template and the
//                           current skin's; vf04 (sub_8224FAD8) releases them
//   player spawn            CActionSpawnPlayer (constructor sub_822114E8) keeps the current skin's hash
//                           (+8) and spawns that template
//   Crash's fight tree      HLSkinWeapon: ContentX branches, Crash:CheckSkinType(ESkin) (sub_821534A0 =
//                           sub_82215730(skin) == current) -> self:SetWeaponType("Crash_X")
//   cutscenes               CCinematicActorAction::vf04 (sub_821B6388) names Crash's NIS actor
//                           current hash + "_nis_NN" (Characters:Crash_Ratcicle_nis_01 ...)
//   unlocked list           0x824A8218, 20 ints (-1 = free), saved with the game (sub_82215C08 /
//                           sub_82215C48); IsSkinUnlocked sub_82215900 (the frontend's conditions,
//                           -1 = the HUB's selected doll), next / previous unlocked sub_822159F8
//                           (the skin screen's left / right)
//
// Crash_Hulk (22) was left half done: the template is "Characters:Crash_Hulk" (createtemplateactor,
// GlobalPackages: package ab4cc04a with 344a15f9 = its model), but sub_82215730 names it
// "Characters:Crash_Crash_Hulk", which doesn't exist; its packages lack the collision parts of the
// "crash" object (Crash fell through the floor); the skin screen's preview switch has no case for
// it (crash); it has no _nis templates; and nothing unlocks it (the 20 retail skins fill the list).
// Here: skin 22 hashes to the real template; its physics and intersection behaviours take their
// parts from the normal Crash's template (kept loaded); the skin screen shows it with the game's
// own "CRASH HULK" strings; cutscenes show the normal Crash while it is worn; and with
// [dev] hulk_skin = true it counts as unlocked without taking a slot of the list (the skin screen's
// left / right reach it after the last skin). Yuktopus 18, Uka Uka 19, Nina 20 (no name: the game
// crashes) and Cortex 1 (no template on the disc) are refused.
//
// Switching: in the frontend (HUB, menus) the game's own SelectSkin path; in a level the change
// waits for the next level load (applied just before vf05 loads the templates, so the old skin is
// released by vf04 first and the new one loaded); "skin <name> now" also restarts the level
// (UberManager:ForceReload goes through vf04 / vf05).

#include "dev_tools.h"
#include "dev_console.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../settings/settings.h"
#include "../settings/ui_text.h"
#include "../online/mp.h"
#include "../online/mp_menu.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82215730);  // ESkin -> template hash
extern "C" PPC_FUNC(__imp__sub_82215900);  // IsSkinUnlocked(skin or -1)
extern "C" PPC_FUNC(__imp__sub_822159F8);  // next / previous unlocked skin
extern "C" PPC_FUNC(__imp__sub_8224E068);  // CStreamManager::vf05 (level load)
extern "C" PPC_FUNC(__imp__sub_8224FAD8);  // CStreamManager::vf04 (level unload)
extern "C" PPC_FUNC(__imp__sub_821B6388);  // CCinematicActorAction::vf04
extern "C" PPC_FUNC(__imp__sub_820BBB68);  // CSkinScreenAction: show skin r4 (its preview template, its texts)
PPC_EXTERN_FUNC(sub_822F10F0);             // text item: show string n of its list
extern "C" PPC_FUNC(__imp__sub_82147568);  // CPhysicsBehaviour::vf12 (finds its ellipsoid; r5 = the package entry)
extern "C" PPC_FUNC(__imp__sub_8211EA88);  // CIntersectionBehaviour::vf12 (finds its volume; r5 likewise)
PPC_EXTERN_FUNC(sub_82215730);             // (hooked below)
PPC_EXTERN_FUNC(sub_822846C0);             // the game's name hash (text, seed)
PPC_EXTERN_FUNC(sub_8224CC08);             // the stream manager
PPC_EXTERN_FUNC(sub_8224CEC8);             // stream manager: release a template (by hash)
PPC_EXTERN_FUNC(sub_8224D3C8);             // stream manager: load a template (by hash)
PPC_EXTERN_FUNC(sub_821D9270);             // frontend manager: select skin (load new, release old)
PPC_EXTERN_FUNC(sub_821FB4F0);             // the managers object
PPC_EXTERN_FUNC(sub_821FA440);             // managers: subsystem n (6 = CFrontendManager)
PPC_EXTERN_FUNC(sub_820AFC50);             // the HUB's grid
PPC_EXTERN_FUNC(sub_820B01A0);             // grid item -> its unlockable (the skin)

namespace
{
    constexpr uint32_t UNLOCKED_SKINS = 0x824A8218;  // 20 ints, -1 = free
    constexpr uint32_t UNLOCKED_COUNT = 20;
    constexpr uint32_t CURRENT_SKIN = 0x824A8270;    // template hash

    // The console's replies, in the game's language ("skin <label>" comes before each).
    using settings::Tr;
    using settings::UiText;
    const UiText T_CANT_USE = { " can't be used", " não pode ser usada", " ne peut pas être utilisé", " kann nicht benutzt werden",
                                " no se puede usar", " non può essere usata", " kan niet gebruikt worden" };
    const UiText T_NO_FRONTEND = { "the frontend doesn't exist yet", "o frontend ainda não existe", "le frontend n'existe pas encore",
                                   "das Frontend gibt es noch nicht", "el frontend aún no existe", "il frontend non esiste ancora",
                                   "de frontend bestaat nog niet" };
    const UiText T_CHOSEN = { " chosen: applies in the next level", " escolhida: vale na próxima fase", " choisi : s'applique au prochain niveau",
                              " gewählt: gilt im nächsten Level", " elegido: vale en el próximo nivel", " scelta: vale nel prossimo livello",
                              " gekozen: geldt in het volgende level" };
    const UiText T_RESTARTING = { ": restarting the level", ": reiniciando a fase", " : redémarrage du niveau", ": Level wird neu gestartet",
                                  ": reiniciando el nivel", ": riavvio del livello", ": level wordt herstart" };
    const UiText T_NEXT_LOAD = { ": applies when the next level loads (\"reload\" or \"skin <name> now\" restarts this one)",
                                 ": vale ao carregar a próxima fase (\"reload\" ou \"skin <nome> now\" reinicia esta)",
                                 " : s'applique au chargement du prochain niveau (\"reload\" ou \"skin <nom> now\" redémarre celui-ci)",
                                 ": gilt beim Laden des nächsten Levels (\"reload\" oder \"skin <name> now\" startet dieses neu)",
                                 ": vale al cargar el próximo nivel (\"reload\" o \"skin <nombre> now\" reinicia este)",
                                 ": vale al caricamento del prossimo livello (\"reload\" o \"skin <nome> now\" riavvia questo)",
                                 ": geldt bij het laden van het volgende level (\"reload\" of \"skin <naam> now\" herstart dit)" };
    const UiText T_APPLIED = { " applied", " aplicada", " appliqué", " angewendet", " aplicado", " applicata", " toegepast" };

    struct Skin
    {
        int id;
        const char* name;     // console name (the ESkin_ suffix)
        const char* label;    // shown
        bool wearable;
    };
    const Skin SKINS[] = {
        { 21, "crash", "Crash", true },           { 22, "hulk", "Crash_Hulk (unused)", true },
        { 0, "chiratta", "Chiratta", true },       { 2, "koala", "Koala", true },
        { 3, "monkey", "Monkey", true },           { 4, "rabbit", "Rabbit", true },
        { 5, "bratgirl", "BratGirl", true },       { 6, "spike", "Spike", true },
        { 7, "shurtle", "Shurtle", true },         { 8, "sludge", "Sludge", true },
        { 9, "ratcicle", "Ratcicle", true },       { 10, "battler", "Battler", true },
        { 11, "grizzly", "Grizzly", true },        { 12, "roller", "Roller", true },
        { 13, "parafox", "Parafox", true },        { 14, "stinky", "Stinky", true },
        { 15, "eelectric", "Eelectric", true },    { 16, "shellephant", "Shellephant", true },
        { 17, "scorporilla", "Scorporilla", true }, { 23, "valentine", "Valentine", true },
        { 24, "halloween", "Halloween (Skeleton)", true },
        { 1, "cortex", "Cortex (no model on the disc)", false },
        { 18, "yuktopus", "Yuktopus (no name: crashes the game)", false },
        { 19, "ukauka", "Uka Uka (no name: crashes the game)", false },
        { 20, "nina", "Nina (no name: crashes the game)", false },
    };

    std::atomic<int> g_pending{ -1 };  // the skin for the next level load
    std::atomic<bool> g_inLevel{ false };
    uint32_t g_hulkHash = 0, g_crashHash = 0;

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = char(tolower(uint8_t(c)));
        return text;
    }

    uint32_t GuestString(const char* text)
    {
        uint32_t address = GuestAlloc(strlen(text) + 1);
        if (address)
            memcpy(g_memory.Translate(address), text, strlen(text) + 1);
        return address;
    }

    uint32_t NameHash(PPCContext& ctx, uint8_t* base, const char* text)
    {
        static std::mutex mutex;
        static std::vector<std::pair<std::string, uint32_t>> strings;
        uint32_t address = 0;
        {
            std::lock_guard lock(mutex);
            for (auto& [s, a] : strings)
                if (s == text)
                    address = a;
            if (!address)
            {
                address = GuestString(text);
                strings.emplace_back(text, address);
            }
        }
        PPCContext call = ctx;
        call.r3.u64 = address;
        call.r4.u64 = 0;
        sub_822846C0(call, base);
        return call.r3.u32;
    }

    void Hashes(PPCContext& ctx, uint8_t* base)
    {
        if (!g_hulkHash)
        {
            g_hulkHash = NameHash(ctx, base, "Characters:Crash_Hulk");
            g_crashHash = NameHash(ctx, base, "Characters:Crash");
        }
    }

    // The stream manager's package entry of a template (CStreamManager *0x824A90CC: entries from +4
    // to +8, the template's hash at entry+20), 0 if none.
    uint32_t StreamEntry(uint32_t hash)
    {
        uint32_t manager = LoadBE32(0x824A90CC);
        if (!manager)
            return 0;
        for (uint32_t at = LoadBE32(manager + 4), end = LoadBE32(manager + 8); at < end; at += 4)
        {
            uint32_t entry = LoadBE32(at);
            if (entry && LoadBE32(entry + 20) == hash)
                return entry;
        }
        return 0;
    }

    // Crash_Hulk takes its collision from the normal Crash's template (UseNormalCrashParts), which
    // the game only keeps loaded as player 2's template in a level (and as the current skin): wearing
    // the Hulk from the skin screen releases it. One reference of our own keeps it loaded from the
    // first time the Hulk is chosen on (it is what a level loads anyway).
    void HoldNormalCrash(PPCContext& ctx, uint8_t* base)
    {
        static bool held = false;
        if (held)
            return;
        held = true;
        Hashes(ctx, base);
        static uint32_t hash = 0;
        if (!hash)
            hash = GuestAlloc(4);
        StoreBE32(hash, g_crashHash);
        PPCContext call = ctx;
        sub_8224CC08(call, base);
        call.r4.u64 = hash;
        sub_8224D3C8(call, base);
        Log("[dev] Crash_Hulk: holding the normal Crash's template (its collision)");
    }

    // A behaviour of the Crash_Hulk actor finds its parts in the template's package entry (r5),
    // whose packages (Crash_Hulk's model 344a15f9 and the shared ones) have a "crash" object without
    // "crashEllipsoidShape" / "Interactor_VolumeShape": Crash fell through the floor. The normal
    // Crash's entry ("Characters:Crash", always loaded: player 2) has them.
    void UseNormalCrashParts(PPCContext& ctx, const char* what)
    {
        if (!g_hulkHash || ctx.r5.u32 == 0 || LoadBE32(ctx.r5.u32 + 20) != g_hulkHash)
            return;
        uint32_t crash = StreamEntry(g_crashHash);
        if (!crash)
            return;
        static int logged = 0;
        if (logged++ < 8)
            Log("[dev] Crash_Hulk: %s from the normal Crash's package (entry 0x%08X instead of 0x%08X)", what, crash, ctx.r5.u32);
        ctx.r5.u64 = crash;
    }

    bool HulkUnlocked() { return settings::Get().hulkSkin; }

    // 22 is in the game's own list (dev.unlock_all({ESkin_Crash_Hulk}) puts it there).
    bool HulkInList()
    {
        for (uint32_t i = 0; i < UNLOCKED_COUNT; i++)
            if (int32_t(LoadBE32(UNLOCKED_SKINS + 4 * i)) == dev::skins::HULK)
                return true;
        return false;
    }

    // The skin the HUB's grid has selected (what IsSkinUnlocked(-1) checks), -1 if none.
    int32_t SelectedDollSkin(PPCContext& ctx, uint8_t* base)
    {
        PPCContext call = ctx;
        sub_820AFC50(call, base);
        uint32_t grid = call.r3.u32;
        if (!grid)
            return -1;
        call = ctx;
        call.r3.u64 = grid;
        call.r4.u64 = LoadBE32(grid + 216);
        sub_820B01A0(call, base);
        return call.r3.s32;
    }

    uint32_t FrontendManager(PPCContext& ctx, uint8_t* base)
    {
        PPCContext call = ctx;
        sub_821FB4F0(call, base);
        call.r4.u64 = 6;
        sub_821FA440(call, base);
        return call.r3.u32;
    }
}

namespace dev::skins
{
    int ByName(const std::string& text)
    {
        std::string name = Lower(text);
        if (name.rfind("eskin_", 0) == 0)
            name = name.substr(6);
        if (name == "crash_hulk" || name == "crashhulk")
            name = "hulk";
        if (name == "skeleton")
            name = "halloween";
        if (name == "uka_uka" || name == "uka")
            name = "ukauka";
        for (const Skin& skin : SKINS)
            if (name == skin.name)
                return skin.id;
        char* end = nullptr;
        long number = strtol(name.c_str(), &end, 10);
        if (!name.empty() && end && !*end && number >= 0 && number <= 24)
            return int(number);
        return -1;
    }

    const char* Label(int skin)
    {
        for (const Skin& s : SKINS)
            if (s.id == skin)
                return s.label;
        return "?";
    }

    bool Wearable(int skin)
    {
        for (const Skin& s : SKINS)
            if (s.id == skin)
                return s.wearable;
        return false;
    }

    std::vector<std::string> Names()
    {
        std::vector<std::string> names;
        for (const Skin& s : SKINS)
            if (s.wearable)
                names.push_back(s.name);
        return names;
    }

    std::string List()
    {
        std::string text;
        for (const Skin& s : SKINS)
            text += std::string(text.empty() ? "" : ", ") + s.name + (s.wearable ? "" : " (x)");
        return text;
    }

    int Current(PPCContext& ctx, uint8_t* base)
    {
        Hashes(ctx, base);
        uint32_t current = LoadBE32(CURRENT_SKIN);
        for (const Skin& s : SKINS)
        {
            if (!s.wearable)
                continue;
            PPCContext call = ctx;
            call.r3.u64 = uint32_t(s.id);
            sub_82215730(call, base);
            if (call.r3.u32 == current)
                return s.id;
        }
        return -1;
    }

    int Pending() { return g_pending.load(); }

    // Between CStreamManager vf05 and vf04 (the HUB loads through them too) with a player 1 (the
    // static CGameDatabase 0x824A76A4 +16; null in the HUB and the menus).
    bool InLevel() { return g_inLevel.load() && LoadBE32(0x824A76A4 + 16) != 0; }

    bool UnlockHulk(bool on)
    {
        settings::Values values = settings::Get();
        bool changed = values.hulkSkin != on;
        values.hulkSkin = on;
        settings::Set(values);
        if (changed)
            settings::Save();
        Log("[dev] skin Crash_Hulk %s in the skin menu ([dev] hulk_skin = %s)", on ? "unlocked" : "locked", on ? "true" : "false");
        return changed;
    }

    std::string Select(PPCContext& ctx, uint8_t* base, int skin, bool now, bool frontend)
    {
        if (!Wearable(skin))
            return std::string("skin ") + std::to_string(skin) + " (" + Label(skin) + ")" + Tr(T_CANT_USE);
        Hashes(ctx, base);
        if (skin == HULK)
            HoldNormalCrash(ctx, base);
        if (!g_inLevel || frontend)
        {
            // Frontend: what the skin screen's "wear" does (Frontend:SelectSkin).
            uint32_t manager = FrontendManager(ctx, base);
            if (!manager)
                return Tr(T_NO_FRONTEND);
            PPCContext call = ctx;
            call.r3.u64 = manager;
            call.r4.u64 = uint32_t(skin);
            sub_821D9270(call, base);
            g_pending = -1;
            Log("[dev] skin %d (%s) selected in the frontend", skin, Label(skin));
            return std::string("skin ") + Label(skin) + Tr(T_CHOSEN);
        }
        g_pending = skin;
        Log("[dev] skin %d (%s) pending for the next level load%s", skin, Label(skin), now ? " (restarting the level)" : "");
        if (now)
        {
            RunLua(ctx, base, "Managers.UberManager:ForceReload()", "console_reload");
            return std::string("skin ") + Label(skin) + Tr(T_RESTARTING);
        }
        return std::string("skin ") + Label(skin) + Tr(T_NEXT_LOAD);
    }
}

// ESkin -> template hash. Crash_Hulk gets its real template; the skins with no name (18-20, which
// made the game crash) and Cortex (no template) answer the normal Crash.
PPC_FUNC(sub_82215730)
{
    int32_t skin = ctx.r3.s32;
    if (skin == dev::skins::HULK || (skin >= 18 && skin <= 20) || skin == 1)
    {
        Hashes(ctx, base);
        ctx.r3.u64 = skin == dev::skins::HULK ? g_hulkHash : g_crashHash;
        return;
    }
    __imp__sub_82215730(ctx, base);
}

// MULTIPLAYER's SKIN AND COLOR (mp_menu.h PickerOpen): the skin screen goes through every wearable skin
// but Crash_Hulk, in the skin menu's order, unlocked or not.
namespace
{
    const int PICKER_SKINS[] = { 21, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 23, 24 };

    bool PickerSkin(int skin)
    {
        return std::find(std::begin(PICKER_SKINS), std::end(PICKER_SKINS), skin) != std::end(PICKER_SKINS);
    }
}

// IsSkinUnlocked: Crash_Hulk with [dev] hulk_skin, without a slot of the game's list.
PPC_FUNC(sub_82215900)
{
    int32_t skin = ctx.r3.s32;
    if (mp::menu::PickerOpen() && PickerSkin(skin == -1 ? int32_t(LoadBE32(0x82497508)) : skin))
    {
        ctx.r3.u64 = 1;
        return;
    }
    PPCContext saved = ctx;
    __imp__sub_82215900(ctx, base);
    if (ctx.r3.u32 == 0 && HulkUnlocked())
    {
        if (skin == -1)
            skin = SelectedDollSkin(saved, base);
        if (skin == dev::skins::HULK)
            ctx.r3.u64 = 1;
    }
}

// The skin screen's left / right (r4 = 2: previous): Crash_Hulk comes after the list's last skin.
PPC_FUNC(sub_822159F8)
{
    int32_t skin = ctx.r3.s32;
    bool previous = ctx.r4.s32 == 2;
    if (mp::menu::PickerOpen())
    {
        constexpr int COUNT = int(std::size(PICKER_SKINS));
        int at = int(std::find(std::begin(PICKER_SKINS), std::end(PICKER_SKINS), skin) - std::begin(PICKER_SKINS));
        at = at >= COUNT ? 0 : (at + (previous ? COUNT - 1 : 1)) % COUNT;
        ctx.r3.s64 = PICKER_SKINS[at];
        return;
    }
    if (!HulkUnlocked() || HulkInList())
    {
        __imp__sub_822159F8(ctx, base);
        return;
    }
    int32_t first = int32_t(LoadBE32(UNLOCKED_SKINS)), last = -1;
    for (uint32_t i = 0; i < UNLOCKED_COUNT; i++)
    {
        int32_t value = int32_t(LoadBE32(UNLOCKED_SKINS + 4 * i));
        if (value == -1)
            break;
        last = value;
    }
    if (skin == dev::skins::HULK)
    {
        ctx.r3.s64 = previous ? (last >= 0 ? last : skin) : (first >= 0 ? first : skin);
        return;
    }
    if ((previous && skin == first) || (!previous && skin == last))
    {
        ctx.r3.s64 = dev::skins::HULK;
        return;
    }
    __imp__sub_822159F8(ctx, base);
}

// CStreamManager::vf05, the level load: a skin chosen during the last level goes in before the
// templates are loaded.
PPC_FUNC(sub_8224E068)
{
    int skin = g_pending.exchange(-1);
    if (skin >= 0)
    {
        PPCContext call = ctx;
        call.r3.u64 = uint32_t(skin);
        sub_82215730(call, base);
        StoreBE32(CURRENT_SKIN, call.r3.u32);
        Log("[dev] skin %d (%s) applied at the level load", skin, dev::skins::Label(skin));
        dev::console::Print(std::string("skin ") + dev::skins::Label(skin) + Tr(T_APPLIED), dev::console::Tone::Ok);
    }
    if (g_hulkHash && LoadBE32(CURRENT_SKIN) == g_hulkHash)
        HoldNormalCrash(ctx, base);
    mp::skins::LevelLoad(ctx, base);  // online: the other players' skins too
    __imp__sub_8224E068(ctx, base);
    g_inLevel = true;
}

// CStreamManager::vf04, the level unload.
PPC_FUNC(sub_8224FAD8)
{
    mp::skins::LevelUnload(ctx, base);
    __imp__sub_8224FAD8(ctx, base);
    g_inLevel = false;
}

// The skin screen's preview (sub_820BBB68(screen, skin)): its own switch maps each ESkin to the
// template "Characters:Crash_X" it loads for the 3D preview and to the index of the skin's name /
// description strings; 1, 18-20 and 22 go to "none", which released the previous preview and then
// read the released template (crash) when Crash_Hulk came up with left / right. Crash_Hulk is shown
// here as that switch does the others: the shown skin at 0x82497508 {skin, string index, template
// hash}, the frontend manager's +16640, the template loaded, the strings. Its name strings are the
// game's own item 22 when the lists have one, else the normal Crash's (21).
PPC_FUNC(sub_820BBB68)
{
    if (ctx.r4.s32 != dev::skins::HULK)
    {
        __imp__sub_820BBB68(ctx, base);
        return;
    }
    constexpr uint32_t SHOWN = 0x82497508;
    Hashes(ctx, base);
    HoldNormalCrash(ctx, base);
    uint32_t screen = ctx.r3.u32;
    PPCContext call = ctx;
    if (LoadBE32(SHOWN + 8))
    {
        sub_8224CC08(call, base);
        call.r4.u64 = SHOWN + 8;
        sub_8224CEC8(call, base);
    }
    uint32_t texts[2] = { LoadBE32(screen + 28), LoadBE32(screen + 24) };
    int32_t index = dev::skins::HULK;
    uint32_t counts[2] = {};
    for (int i = 0; i < 2; i++)
    {
        // The strings of a text item: a vector at +112 / +116 (anything implausible: not used).
        uint32_t text = texts[i];
        counts[i] = text ? (LoadBE32(text + 116) - LoadBE32(text + 112)) / 4 : 0;
        if (counts[i] <= uint32_t(dev::skins::HULK) || counts[i] > 64)
            index = dev::skins::CRASH;
    }
    StoreBE32(SHOWN, uint32_t(dev::skins::HULK));
    StoreBE32(SHOWN + 4, uint32_t(index));
    StoreBE32(SHOWN + 8, g_hulkHash);
    StoreBE32(FrontendManager(ctx, base) + 16640, uint32_t(dev::skins::HULK));
    call = ctx;
    sub_8224CC08(call, base);
    call.r4.u64 = SHOWN + 8;
    sub_8224D3C8(call, base);
    for (uint32_t text : texts)
    {
        if (!text)
            continue;
        call = ctx;
        call.r3.u64 = text;
        call.r4.u64 = uint32_t(index);
        sub_822F10F0(call, base);
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("[dev] skin screen: Crash_Hulk shown (name strings %d; the lists have %u / %u)", index, counts[0], counts[1]);
    }
}

// Crash_Hulk's collision: see UseNormalCrashParts.
PPC_FUNC(sub_82147568)
{
    UseNormalCrashParts(ctx, "physics ellipsoid");
    __imp__sub_82147568(ctx, base);
}

PPC_FUNC(sub_8211EA88)
{
    UseNormalCrashParts(ctx, "intersection volume");
    __imp__sub_8211EA88(ctx, base);
}

// Crash's cutscene actor: Crash_Hulk has no _nis templates, the normal Crash plays them.
PPC_FUNC(sub_821B6388)
{
    Hashes(ctx, base);
    uint32_t current = LoadBE32(CURRENT_SKIN);
    if (current != g_hulkHash)
    {
        __imp__sub_821B6388(ctx, base);
        return;
    }
    StoreBE32(CURRENT_SKIN, g_crashHash);
    __imp__sub_821B6388(ctx, base);
    StoreBE32(CURRENT_SKIN, current);
}
