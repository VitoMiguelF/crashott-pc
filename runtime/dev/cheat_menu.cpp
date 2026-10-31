// The cheat menu (cheat_menu.h): the code in the pause, the menu's input and actions on the game
// thread, its drawing on the render thread (a CPU canvas in the game's style, like the save screens'
// question, through the overlay's BITMAP_CHEATS). Everything both threads read is under g_mutex.

#include "cheat_menu.h"
#include "dev_tools.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../gpu/png_writer.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "../online/mp.h"
#include "../platform/keys.h"
#include "../platform/platform.h"
#include "../settings/menu_art.h"
#include "../settings/prompt_icons.h"
#include "../settings/settings.h"
#include "../settings/settings_menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

PPC_EXTERN_FUNC(sub_821FB4F0);  // the managers; sub_821FA440(them, 6) = the frontend sound player
PPC_EXTERN_FUNC(sub_821FA440);
PPC_EXTERN_FUNC(sub_821DB1D8);  // sound player: play (bank r4, sound r5, volume f1)
PPC_EXTERN_FUNC(sub_821DB260);  // the menus' "previous" move sound
PPC_EXTERN_FUNC(sub_821DB280);  // the menus' "next" move sound

namespace dev::cheat_menu
{
    namespace
    {
        enum Item { INVINCIBLE, LIFE, ONE_HIT_TITAN, NO_AI, UNLOCK_MOVES, UNLOCK_SKINS, UNLOCK_EPISODES, UNLOCK_ALL, ITEMS };

        bool IsToggle(int item) { return item == INVINCIBLE || item == ONE_HIT_TITAN || item == NO_AI; }
        bool IsUnlock(int item) { return item >= UNLOCK_MOVES; }

        // ---- texts -------------------------------------------------------------------------------
        // The items in upper case (the game's menus), the lines under them in the save messages' case.
        // "\xC2\xB1" / "\xC2\xB2" are the fonts' A / B icons (menu_art.h ICON_A, ICON_B).
        struct Texts
        {
            const char* title;
            const char* items[ITEMS];
            const char* lines[ITEMS];
            const char *on, *off, *unlocked, *permanent, *confirm, *lives;
            const char* note;
            const char *back, *select;
        };

        const Texts ENGLISH = {
            "CHEATS",
            { "INVINCIBILITY", "+1 LIFE", "1 HIT TITAN", "DISABLE AI", "UNLOCK ALL MOVES", "UNLOCK ALL SKINS", "UNLOCK ALL EPISODES",
              "UNLOCK EVERYTHING" },
            { "Crash takes no damage (falls still count).", "One more life, now.", "A single hit stuns a titan.",
              "The enemies stop thinking and stand still.", "Every move and upgrade.", "The game's 20 skins and Crash Hulk.",
              "Every episode in the HUB.", "Skins, episodes and moves." },
            "ON", "OFF", "UNLOCKED", "PERMANENT", "\xC2\xB1 CONFIRM", "LIVES: %d",
            "Unlocks are permanent: the game writes them to your save when it saves. Cheats that are on last until you quit the game.",
            "BACK", "SELECT",
        };

        const Texts PORTUGUESE = {
            "TRAPAÇAS",
            { "INVENCIBILIDADE", "+1 VIDA", "TITÃ EM 1 GOLPE", "DESATIVAR IA", "DESBLOQUEAR TODOS OS GOLPES", "DESBLOQUEAR TODAS AS SKINS",
              "DESBLOQUEAR TODOS OS EPISÓDIOS", "DESBLOQUEAR TUDO" },
            { "Crash não sofre dano (as quedas ainda contam).", "Mais uma vida, agora.", "Um único golpe atordoa um titã.",
              "Os inimigos param de pensar e ficam parados.", "Todos os golpes e melhorias.", "As 20 skins do jogo e o Crash Hulk.",
              "Todos os episódios do HUB.", "Skins, episódios e golpes." },
            "LIGADO", "DESLIGADO", "DESBLOQUEADO", "PERMANENTE", "\xC2\xB1 CONFIRMAR", "VIDAS: %d",
            "Os desbloqueios são permanentes: o jogo os grava no save quando salvar. As trapaças ligadas valem até você fechar o jogo.",
            "VOLTAR", "SELECIONAR",
        };

        const Texts FRENCH = {
            "TRICHES",
            { "INVINCIBILITÉ", "+1 VIE", "TITAN EN 1 COUP", "DÉSACTIVER L'IA", "DÉBLOQUER TOUS LES COUPS", "DÉBLOQUER TOUS LES COSTUMES",
              "DÉBLOQUER TOUS LES ÉPISODES", "TOUT DÉBLOQUER" },
            { "Crash ne subit aucun dégât (les chutes comptent toujours).", "Une vie de plus, tout de suite.", "Un seul coup étourdit un titan.",
              "Les ennemis cessent de réfléchir et restent immobiles.", "Tous les coups et améliorations.", "Les 20 costumes du jeu et Crash Hulk.",
              "Tous les épisodes du HUB.", "Costumes, épisodes et coups." },
            "ACTIVÉ", "DÉSACTIVÉ", "DÉBLOQUÉ", "DÉFINITIF", "\xC2\xB1 CONFIRMER", "VIES : %d",
            "Les déblocages sont définitifs : le jeu les écrit dans votre sauvegarde quand il sauvegarde. Les triches activées durent jusqu'à ce que vous quittiez le jeu.",
            "RETOUR", "SÉLECTIONNER",
        };

        const Texts GERMAN = {
            "CHEATS",
            { "UNVERWUNDBARKEIT", "+1 LEBEN", "TITAN MIT 1 TREFFER", "KI DEAKTIVIEREN", "ALLE ANGRIFFE FREISCHALTEN", "ALLE SKINS FREISCHALTEN",
              "ALLE EPISODEN FREISCHALTEN", "ALLES FREISCHALTEN" },
            { "Crash nimmt keinen Schaden (Stürze zählen weiterhin).", "Sofort ein Leben mehr.", "Ein einziger Treffer betäubt einen Titanen.",
              "Die Gegner denken nicht mehr nach und bleiben stehen.", "Alle Angriffe und Verbesserungen.", "Die 20 Skins des Spiels und Crash Hulk.",
              "Alle Episoden im HUB.", "Skins, Episoden und Angriffe." },
            "AN", "AUS", "FREIGESCHALTET", "DAUERHAFT", "\xC2\xB1 BESTÄTIGEN", "LEBEN: %d",
            "Freischaltungen sind dauerhaft: Das Spiel schreibt sie beim Speichern in deinen Spielstand. Aktive Cheats bleiben an, bis du das Spiel beendest.",
            "ZURÜCK", "AUSWÄHLEN",
        };

        const Texts SPANISH = {
            "TRUCOS",
            { "INVENCIBILIDAD", "+1 VIDA", "TITÁN DE 1 GOLPE", "DESACTIVAR IA", "DESBLOQUEAR TODOS LOS GOLPES", "DESBLOQUEAR TODOS LOS TRAJES",
              "DESBLOQUEAR TODOS LOS EPISODIOS", "DESBLOQUEARLO TODO" },
            { "Crash no recibe daño (las caídas siguen contando).", "Una vida más, ahora mismo.", "Un solo golpe aturde a un titán.",
              "Los enemigos dejan de pensar y se quedan quietos.", "Todos los golpes y mejoras.", "Los 20 trajes del juego y Crash Hulk.",
              "Todos los episodios del HUB.", "Trajes, episodios y golpes." },
            "ACTIVADO", "DESACTIVADO", "DESBLOQUEADO", "PERMANENTE", "\xC2\xB1 CONFIRMAR", "VIDAS: %d",
            "Los desbloqueos son permanentes: el juego los guarda en tu partida cuando guarda. Los trucos activados duran hasta que cierres el juego.",
            "ATRÁS", "SELECCIONAR",
        };

        const Texts ITALIAN = {
            "TRUCCHI",
            { "INVINCIBILITÀ", "+1 VITA", "TITANO IN 1 COLPO", "DISATTIVA IA", "SBLOCCA TUTTE LE MOSSE", "SBLOCCA TUTTI I COSTUMI",
              "SBLOCCA TUTTI GLI EPISODI", "SBLOCCA TUTTO" },
            { "Crash non subisce danni (le cadute contano ancora).", "Una vita in più, subito.", "Un solo colpo stordisce un titano.",
              "I nemici smettono di pensare e restano fermi.", "Tutte le mosse e i potenziamenti.", "I 20 costumi del gioco e Crash Hulk.",
              "Tutti gli episodi dell'HUB.", "Costumi, episodi e mosse." },
            "ATTIVO", "DISATTIVO", "SBLOCCATO", "PERMANENTE", "\xC2\xB1 CONFERMA", "VITE: %d",
            "Gli sblocchi sono permanenti: il gioco li scrive nel salvataggio quando salva. I trucchi attivi durano finché non chiudi il gioco.",
            "INDIETRO", "SELEZIONA",
        };

        const Texts DUTCH = {
            "CHEATS",
            { "ONKWETSBAARHEID", "+1 LEVEN", "TITAN IN 1 KLAP", "AI UITSCHAKELEN", "ALLE AANVALLEN VRIJSPELEN", "ALLE SKINS VRIJSPELEN",
              "ALLE AFLEVERINGEN VRIJSPELEN", "ALLES VRIJSPELEN" },
            { "Crash krijgt geen schade (vallen telt nog steeds).", "Meteen een leven extra.", "Eén klap verdooft een titan.",
              "De vijanden denken niet meer na en blijven staan.", "Alle aanvallen en upgrades.", "De 20 skins van het spel en Crash Hulk.",
              "Alle afleveringen in de HUB.", "Skins, afleveringen en aanvallen." },
            "AAN", "UIT", "VRIJGESPEELD", "PERMANENT", "\xC2\xB1 BEVESTIGEN", "LEVENS: %d",
            "Vrijgespeelde onderdelen zijn permanent: het spel schrijft ze in je opgeslagen spel wanneer het opslaat. Actieve cheats blijven aan tot je het spel afsluit.",
            "TERUG", "SELECTEREN",
        };

        const Texts* g_texts = nullptr;  // Preview sets each

        // The language the game runs in (settings.h: a change applies after a restart).
        const Texts& T()
        {
            static const Texts* texts = [] {
                std::string id = settings::StartupLanguage().id;
                if (id.rfind("pt", 0) == 0)
                    return &PORTUGUESE;
                if (id == "french")
                    return &FRENCH;
                if (id == "german")
                    return &GERMAN;
                if (id == "spanish")
                    return &SPANISH;
                if (id == "italian")
                    return &ITALIAN;
                if (id == "dutch")
                    return &DUTCH;
                return &ENGLISH;
            }();
            return g_texts ? *g_texts : *texts;
        }

        // ---- state (game thread, published to the render thread) ---------------------------------
        struct Visual
        {
            bool open = false;
            int selected = 0;
            int confirm = -1;  // the unlock waiting for its second A
            bool on[ITEMS] = {};
            bool done[ITEMS] = {};
            int lives = -1;
            bool operator==(const Visual& o) const
            {
                return open == o.open && selected == o.selected && confirm == o.confirm && lives == o.lives && !memcmp(on, o.on, sizeof(on)) &&
                       !memcmp(done, o.done, sizeof(done));
            }
        };

        // Where the mouse can point, in the 1280 x 720 image (made when the menu is drawn).
        enum class Zone { Row, Back, Accept };
        struct HitZone
        {
            float x0, y0, x1, y1;
            Zone zone;
            int index;
        };

        enum class Action { Life, UnlockMoves, UnlockSkins, UnlockEpisodes, UnlockAll, SoundSelect, SoundBack, SoundNo, SoundNext, SoundPrevious, Lives };

        std::mutex g_mutex;
        Visual g_visual;  // under g_mutex
        std::vector<HitZone> g_zones;  // under g_mutex
        std::deque<Action> g_actions;  // under g_mutex

        std::atomic<bool> g_open{ false }, g_releaseHold{ false };
        int g_selected = 0, g_confirm = -1;
        bool g_done[ITEMS] = {};
        int g_lives = -1;
        uint16_t g_lastButtons = 0;
        uint64_t g_heldSince = 0, g_lastRepeat = 0;
        uint16_t g_heldDir = 0;
        uint32_t g_mouseMoves = 0, g_mouseClicks = 0;
        uint64_t g_closedAt = 0;
        // The code.
        int g_codeStep = 0;
        int g_tapStep = 0;
        uint64_t g_tapStart = 0;
        uint32_t g_tapClicks = 0;

        constexpr uint16_t DIRECTIONS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
        constexpr uint16_t FACE = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y;
        constexpr uint16_t CODE[] = { XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_X };
        constexpr uint64_t RELEASE_MS = 150;

        void Queue(Action action)
        {
            std::lock_guard lock(g_mutex);
            g_actions.push_back(action);
        }

        void Publish()
        {
            Visual v;
            v.open = g_open;
            v.selected = g_selected;
            v.confirm = g_confirm;
            v.on[INVINCIBLE] = Invincible();
            v.on[ONE_HIT_TITAN] = OneHitTitans();
            v.on[NO_AI] = AiDisabled();
            memcpy(v.done, g_done, sizeof(g_done));
            v.lives = g_lives;
            std::lock_guard lock(g_mutex);
            g_visual = v;
        }

        // The pause screens of the frontend tree (docs/internals/frontend-and-boot.md): 1081 the
        // pause, 1109 the pause without saving, 1134 the demo's.
        bool InPause()
        {
            uint32_t node = settings::menu::FrontendNode();
            return node == 1081 || node == 1109 || node == 1134;
        }

        void Open(const char* how)
        {
            g_open = true;
            g_selected = 0;
            g_confirm = -1;
            g_codeStep = g_tapStep = 0;
            g_mouseMoves = input::pc::CursorMoves(), g_mouseClicks = input::pc::LeftClicks();
            g_heldDir = 0;
            Log("[cheats] menu opened (%s)", how);
            Queue(Action::Lives);
            Queue(Action::SoundSelect);
            Publish();
        }

        void Close(const char* why)
        {
            g_open = false;
            g_releaseHold = true;
            g_closedAt = plat::TickCountMs();
            g_confirm = -1;
            Log("[cheats] menu closed (%s)", why);
            Publish();
        }

        void Activate(int item)
        {
            if (IsToggle(item))
            {
                bool on;
                if (item == INVINCIBLE)
                    SetInvincible(on = !Invincible());
                else if (item == ONE_HIT_TITAN)
                    SetOneHitTitans(on = !OneHitTitans());
                else
                    SetAiDisabled(on = !AiDisabled());
                Queue(on ? Action::SoundSelect : Action::SoundBack);
            }
            else if (item == LIFE)
                Queue(Action::Life);
            else if (g_done[item])
                Queue(Action::SoundNo);
            else if (g_confirm != item)
            {
                // Permanent: the first A only asks.
                g_confirm = item;
                Queue(Action::SoundNext);
            }
            else
            {
                g_confirm = -1;
                g_done[item] = true;
                if (item == UNLOCK_ALL)
                    g_done[UNLOCK_MOVES] = g_done[UNLOCK_SKINS] = g_done[UNLOCK_EPISODES] = true;
                if (g_done[UNLOCK_MOVES] && g_done[UNLOCK_SKINS] && g_done[UNLOCK_EPISODES])
                    g_done[UNLOCK_ALL] = true;
                Queue(item == UNLOCK_MOVES ? Action::UnlockMoves : item == UNLOCK_SKINS ? Action::UnlockSkins
                                                               : item == UNLOCK_EPISODES ? Action::UnlockEpisodes : Action::UnlockAll);
                Queue(Action::SoundSelect);
            }
            Log("[cheats] %s", ENGLISH.items[item]);
        }

        void Select(int item)
        {
            if (item == g_selected)
                return;
            Queue(item > g_selected ? Action::SoundNext : Action::SoundPrevious);
            g_selected = item;
            g_confirm = -1;
        }

        bool ZoneAt(float x, float y, HitZone& out)
        {
            std::lock_guard lock(g_mutex);
            for (const HitZone& zone : g_zones)
            {
                if (x >= zone.x0 && x < zone.x1 && y >= zone.y0 && y < zone.y1)
                {
                    out = zone;
                    return true;
                }
            }
            return false;
        }

        // The code, in the pause only: the controller's (RT held), the keyboard's (Shift held) or the
        // taps in the top corners.
        void WatchCode(const XINPUT_GAMEPAD& pad, uint16_t pressed, uint64_t now)
        {
            if (!InPause() || mp::Online())
            {
                g_codeStep = g_tapStep = 0;
                g_tapClicks = input::pc::LeftClicks();
                return;
            }
            bool trigger = pad.bRightTrigger > 128 || input::pc::KeyHeld(VK_LSHIFT) || input::pc::KeyHeld(VK_RSHIFT);
            if (!trigger)
                g_codeStep = 0;
            else if (pressed & (FACE | DIRECTIONS | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK))
            {
                if (pressed == CODE[g_codeStep])
                    g_codeStep++;
                else
                    g_codeStep = pressed == CODE[0] ? 1 : 0;
                if (g_codeStep == int(std::size(CODE)))
                {
                    Open(pad.bRightTrigger > 128 ? "RT + Y Y X X" : "Shift + Tab Tab Backspace Backspace");
                    return;
                }
            }

            uint32_t clicks = input::pc::LeftClicks();
            if (clicks == g_tapClicks)
                return;
            g_tapClicks = clicks;
            float x, y;
            int corner = -1;  // 0 top left, 1 top right
            if (input::pc::CursorOnImage(x, y) && y < 160.0f)
                corner = x < 200.0f ? 0 : x > 1080.0f ? 1 : -1;
            if (g_tapStep && now - g_tapStart > 5000)
                g_tapStep = 0;
            if (corner == (g_tapStep & 1))
            {
                if (g_tapStep++ == 0)
                    g_tapStart = now;
            }
            else
                g_tapStep = corner == 0 ? 1 : 0, g_tapStart = now;
            if (g_tapStep == 4)
                Open("taps in the top corners");
        }

        // The menu's input (open).
        void Drive(uint16_t buttons, uint16_t pressed, uint64_t now)
        {
            if (!InPause())
            {
                Close("the pause closed");
                return;
            }
            // Up / down repeat while held.
            uint16_t dir = buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);
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
            if (step & XINPUT_GAMEPAD_DPAD_UP)
                Select((g_selected + ITEMS - 1) % ITEMS);
            else if (step & XINPUT_GAMEPAD_DPAD_DOWN)
                Select((g_selected + 1) % ITEMS);

            // The mouse / the touch screen: pointing selects, a click does it.
            uint32_t moves = input::pc::CursorMoves(), clicks = input::pc::LeftClicks();
            bool moved = moves != g_mouseMoves, clicked = clicks != g_mouseClicks;
            g_mouseMoves = moves, g_mouseClicks = clicks;
            input::pc::TakeMenuClick();
            input::pc::TakeMenuWheel();
            float x, y;
            HitZone zone{};
            bool onZone = (moved || clicked) && input::pc::CursorOnImage(x, y) && ZoneAt(x, y, zone);
            if (onZone && zone.zone == Zone::Row && (moved || clicked))
                Select(zone.index);

            if ((pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) || (clicked && onZone && zone.zone == Zone::Back))
            {
                Queue(Action::SoundBack);
                Close(clicked ? "click on back" : "B");
            }
            else if ((pressed & XINPUT_GAMEPAD_A) || (clicked && onZone))
                Activate(g_selected);
            Publish();
        }
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
            uint16_t pressed = buttons & ~g_lastButtons;
            g_lastButtons = buttons;
            if (g_open)
                Drive(buttons, pressed, now);
            else if (g_releaseHold)
            {
                if (!buttons && pad.bRightTrigger < 32 && pad.bLeftTrigger < 32 && now - g_closedAt > RELEASE_MS)
                    g_releaseHold = false;
                g_codeStep = 0;
            }
            else
                WatchCode(pad, pressed, now);
        }
        if (BlocksGameInput())
            pad = {};
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
        if (actions.empty())
            return;
        auto sound = [&](const char* name) {
            constexpr uint32_t BANK_FE = 0x82006DA4;  // "FE_SFX"
            constexpr uint32_t VOLUME = 0x82000DC8;   // the float the frontend's sounds play at
            uint32_t address = strcmp(name, "back") == 0 ? 0x82006DAC : strcmp(name, "select") == 0 ? 0x82006DB4 : 0x8200757C;  // "backpackNONONO"
            PPCContext call = ctx;
            sub_821FB4F0(call, base);
            call.r4.s64 = 6;
            sub_821FA440(call, base);
            uint32_t bits = LoadBE32(VOLUME);
            float volume;
            memcpy(&volume, &bits, 4);
            call.r4.u64 = BANK_FE;
            call.r5.u64 = address;
            call.f1.f64 = volume;
            sub_821DB1D8(call, base);
        };
        auto lua = [&](const char* code) {
            std::string error;
            if (!RunLua(ctx, base, code, "cheats", &error))
                Log("[cheats] %s failed: %s", code, error.c_str());
        };
        bool lives = false;
        for (Action action : actions)
        {
            switch (action)
            {
            case Action::Life:
                g_lives = AddLife(ctx, base);
                sound(g_lives >= 0 ? "select" : "no");
                break;
            case Action::UnlockMoves: lua("dev.unlock_moves()"); break;
            case Action::UnlockSkins:
                lua("dev.unlock_skins()");
                skins::UnlockHulk(true);
                break;
            case Action::UnlockEpisodes: lua("dev.unlock_levels()"); break;
            case Action::UnlockAll:
                lua("dev.unlock_all()");
                skins::UnlockHulk(true);
                break;
            case Action::SoundSelect: sound("select"); break;
            case Action::SoundBack: sound("back"); break;
            case Action::SoundNo: sound("no"); break;
            case Action::SoundNext:
            case Action::SoundPrevious:
            {
                PPCContext call = ctx;
                sub_821FB4F0(call, base);
                call.r4.s64 = 6;
                sub_821FA440(call, base);
                if (action == Action::SoundNext)
                    sub_821DB280(call, base);
                else
                    sub_821DB260(call, base);
                break;
            }
            case Action::Lives: lives = true; break;
            }
        }
        if (lives)
            g_lives = Lives(ctx, base);
        Publish();
    }
}

// ---- drawing (render thread) -------------------------------------------------------------------

namespace dev::cheat_menu
{
    namespace
    {
        using settings::menu_art::Color;
        using settings::menu_art::FontId;
        using settings::menu_art::TextStyle;
        namespace menu_art = settings::menu_art;

        constexpr float VIRTUAL_W = 1280.0f, VIRTUAL_H = 720.0f;

        // The save screens' colours over a message frame (save_screens.cpp).
        constexpr TextStyle MSG_TEXT{ { 238, 226, 196, 255 }, { 52, 28, 4, 255 } };
        constexpr TextStyle MSG_ITEM{ { 232, 160, 0, 255 }, { 104, 48, 8, 255 } };
        constexpr TextStyle MSG_SELECTED{ { 144, 216, 232, 255 }, { 24, 104, 128, 255 } };
        constexpr TextStyle MSG_WARNING{ { 255, 128, 72, 255 }, { 96, 24, 0, 255 } };
        constexpr TextStyle KEY_NORMAL{ { 214, 120, 14, 255 }, { 70, 28, 0, 255 } };
        constexpr TextStyle STATE_ON{ { 150, 232, 96, 255 }, { 28, 84, 12, 255 } };
        constexpr TextStyle STATE_OFF{ { 176, 160, 136, 255 }, { 52, 36, 20, 255 } };
        constexpr Color FRAME{ 236, 222, 190, 190 }, DIM{ 14, 8, 0, 150 };

        struct View
        {
            float s, ox, oy;
            float X(float x) const { return ox + x * s; }
            float Y(float y) const { return oy + y * s; }
            int IX(float x) const { return int(std::lround(X(x))); }
            int IY(float y) const { return int(std::lround(Y(y))); }
        };

        menu_art::Canvas g_canvas;
        std::vector<uint32_t> g_pixels;
        Visual g_drawn;
        uint32_t g_drawnStyle = 0;
        int g_drawnW = 0, g_drawnH = 0;
        uint64_t g_version = 0;
        std::vector<HitZone> g_newZones;

        float Width(FontId font, const std::string& text, float cell)
        {
            return menu_art::TextWidth(font, menu_art::Utf32(text), cell);
        }

        float Text(const View& v, FontId font, const std::string& text, float x, float y, float cell, const TextStyle& style, float align = 0.0f,
                   float maxWidth = 1e9f)
        {
            float fitted = cell;
            std::u32string u = menu_art::FitText(font, menu_art::Utf32(text), fitted, maxWidth, 0.7f);
            y += (cell - fitted) * 0.5f, cell = fitted;
            float width = menu_art::TextWidth(font, u, cell);
            float left = x - width * align;
            menu_art::DrawGameText(g_canvas, font, u, v.X(left), v.Y(y), cell * v.s, style);
            return left + width;
        }

        // Words of `text` in lines no wider than `width`.
        std::vector<std::string> Wrap(const std::string& text, FontId font, float cell, float width)
        {
            std::vector<std::string> lines;
            std::string line;
            size_t at = 0;
            while (at < text.size())
            {
                size_t end = text.find(' ', at);
                std::string word = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? text.size() : end + 1;
                std::string tried = line.empty() ? word : line + " " + word;
                if (!line.empty() && Width(font, tried, cell) > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                    line = tried;
            }
            if (!line.empty())
                lines.push_back(line);
            return lines;
        }

        // The title, the eight items with their state at the right, the selected item's line, the
        // note about the unlocks and the hints, in a message frame at the middle of the screen.
        void Rasterize(const Visual& vis, int width, int height)
        {
            View v;
            v.s = std::min(width / VIRTUAL_W, height / VIRTUAL_H);
            v.ox = (width - VIRTUAL_W * v.s) * 0.5f, v.oy = (height - VIRTUAL_H * v.s) * 0.5f;
            g_canvas.Reset(width, height);
            g_newZones.clear();
            g_canvas.Fill(0, 0, width, height, DIM);

            const Texts& t = T();
            constexpr float TITLE = 44.0f, CELL = 32.0f, PITCH = 40.0f, STATE = 26.0f, LINE = 24.0f, NOTE = 21.0f, NOTE_PITCH = 25.0f, HINT = 28.0f;
            constexpr float CONTENT_W = 900.0f, GAP = 10.0f;
            std::vector<std::string> note = Wrap(t.note, FontId::Small, NOTE, CONTENT_W);
            const float contentH = TITLE + 12.0f + PITCH * float(ITEMS) + 8.0f + LINE + 12.0f + NOTE_PITCH * note.size() + 14.0f + HINT;
            float frameW, frameH;
            menu_art::FrameOuterSize(CONTENT_W, contentH, GAP, frameW, frameH);
            frameW = std::min(frameW, 1240.0f), frameH = std::min(frameH, 716.0f);
            const float Y0 = std::round((VIRTUAL_H - frameH) * 0.5f), Y1 = Y0 + frameH;
            const float X0 = std::round((VIRTUAL_W - frameW) * 0.5f), X1 = VIRTUAL_W - X0;
            menu_art::DrawMessageFrame(g_canvas, v.IX(X0), v.IY(Y0), v.IX(X1), v.IY(Y1), FRAME, { 0, 0, 0, 190 });
            auto inset = [&](float y, float cell) { return menu_art::FrameSideInset(X0, Y0, X1, Y1, y, y + cell) + GAP; };
            const float cx = VIRTUAL_W * 0.5f;

            float y = Y0 + menu_art::FrameTopInset(Y0, Y1) + GAP;
            Text(v, FontId::Large, t.title, cx, y, TITLE, MSG_TEXT, 0.5f);
            y += TITLE + 12.0f;

            char lives[64] = "";
            if (vis.lives >= 0)
                snprintf(lives, sizeof(lives), t.lives, vis.lives);
            // One column for every row: clear of the curls over the whole list.
            const float listSide = menu_art::FrameSideInset(X0, Y0, X1, Y1, y, y + PITCH * float(ITEMS)) + GAP;
            for (int i = 0; i < ITEMS; i++)
            {
                bool selected = vis.selected == i;
                float left = X0 + listSide, right = X1 - listSide;
                std::string state;
                const TextStyle* stateStyle = &STATE_OFF;
                if (IsToggle(i))
                    state = vis.on[i] ? t.on : t.off, stateStyle = vis.on[i] ? &STATE_ON : &STATE_OFF;
                else if (i == LIFE)
                    state = lives;
                else if (vis.done[i])
                    state = t.unlocked, stateStyle = &STATE_ON;
                else if (vis.confirm == i)
                    state = t.confirm, stateStyle = &MSG_WARNING;
                else
                    state = t.permanent, stateStyle = &MSG_WARNING;
                float stateW = state.empty() ? 0.0f : std::min(Width(FontId::Large, state, STATE), 300.0f);
                if (!state.empty())
                    Text(v, FontId::Large, state, right, y + (CELL - STATE) * 0.5f, STATE, *stateStyle, 1.0f, 300.0f);
                float cell = selected ? CELL : CELL - 3.0f;
                Text(v, FontId::Large, t.items[i], left, y + (CELL - cell) * 0.5f, cell, selected ? MSG_SELECTED : MSG_ITEM, 0.0f,
                     right - left - stateW - 24.0f);
                g_newZones.push_back({ left - 8.0f, y - (PITCH - CELL) * 0.5f, right + 8.0f, y + CELL + (PITCH - CELL) * 0.5f, Zone::Row, i });
                y += PITCH;
            }
            y += 8.0f;
            Text(v, FontId::Small, t.lines[vis.selected], cx, y, LINE, MSG_SELECTED, 0.5f, X1 - X0 - 2.0f * inset(y, LINE));
            y += LINE + 12.0f;
            for (const std::string& line : note)
            {
                Text(v, FontId::Small, line, cx, y, NOTE, MSG_WARNING, 0.5f, X1 - X0 - 2.0f * inset(y, NOTE));
                y += NOTE_PITCH;
            }
            y += 14.0f;

            // (B) BACK ... SELECT (A), as the game's hints (with the device's icons).
            std::string back = std::string("\xC2\xB2 ") + t.back, select = std::string(t.select) + " \xC2\xB1";
            float side = inset(y, HINT);
            float backEnd = Text(v, FontId::Large, back, X0 + side, y, HINT, KEY_NORMAL);
            Text(v, FontId::Large, select, X1 - side, y, HINT, KEY_NORMAL, 1.0f);
            float selectW = Width(FontId::Large, select, HINT);
            g_newZones.push_back({ X0 + side - 10.0f, y - 6.0f, backEnd + 10.0f, y + HINT + 6.0f, Zone::Back, 0 });
            g_newZones.push_back({ X1 - side - selectW - 10.0f, y - 6.0f, X1 - side + 10.0f, y + HINT + 6.0f, Zone::Accept, 0 });

            std::lock_guard lock(g_mutex);
            g_zones = g_newZones;
        }
    }

    void Draw(uint32_t width, uint32_t height)
    {
        Visual vis;
        {
            std::lock_guard lock(g_mutex);
            vis = g_visual;
        }
        if (!vis.open || !width || !height || !menu_art::Ready(0))
            return;
        uint32_t style = settings::prompts::Style();
        if (int(width) != g_drawnW || int(height) != g_drawnH)
        {
            g_drawnW = int(width), g_drawnH = int(height);
            g_drawn = {};
        }
        if (!(vis == g_drawn) || style != g_drawnStyle)
        {
            Rasterize(vis, int(width), int(height));
            g_canvas.Export(g_pixels);
            g_drawn = vis;
            g_drawnStyle = style;
            g_version++;
        }
        gpu::overlay::DrawBitmap(width, height, g_pixels.data(), int(width), int(height), 0, 0, g_version, gpu::overlay::BITMAP_CHEATS);
    }

    int Preview(const std::filesystem::path& directory)
    {
        if (!menu_art::Ready(10000))
        {
            printf("cheats preview: the game's fonts are missing\n");
            return 1;
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        struct Language
        {
            const char* id;
            const Texts* texts;
        };
        const Language languages[] = { { "english", &ENGLISH }, { "pt-br", &PORTUGUESE }, { "french", &FRENCH }, { "german", &GERMAN },
                                       { "spanish", &SPANISH }, { "italian", &ITALIAN }, { "dutch", &DUTCH } };
        struct Size
        {
            int w, h;
        };
        int written = 0;
        for (const Language& language : languages)
        {
            g_texts = language.texts;
            for (Size size : { Size{ 1280, 720 }, Size{ 1920, 1080 }, Size{ 2560, 1080 } })
            {
                // As opened; a toggle selected with the three on and the lives known; an unlock asking
                // for its second A; everything unlocked.
                for (int shot = 0; shot < 4; shot++)
                {
                    Visual vis;
                    vis.open = true;
                    vis.lives = shot ? 4 : -1;
                    if (shot >= 1)
                        vis.on[INVINCIBLE] = vis.on[ONE_HIT_TITAN] = vis.on[NO_AI] = true;
                    vis.selected = shot == 1 ? ONE_HIT_TITAN : shot == 2 ? UNLOCK_SKINS : shot == 3 ? UNLOCK_ALL : 0;
                    vis.confirm = shot == 2 ? UNLOCK_SKINS : -1;
                    if (shot == 3)
                        vis.done[UNLOCK_MOVES] = vis.done[UNLOCK_SKINS] = vis.done[UNLOCK_EPISODES] = vis.done[UNLOCK_ALL] = true;
                    Rasterize(vis, size.w, size.h);
                    g_canvas.Export(g_pixels);
                    // Over a dark green, like a level behind the pause.
                    const uint32_t back[3] = { 30, 70, 40 };
                    std::vector<uint32_t> image(g_pixels.size());
                    for (size_t i = 0; i < image.size(); i++)
                    {
                        uint32_t p = g_pixels[i], a = p >> 24;
                        auto mix = [&](uint32_t c, uint32_t b) { return (c * a + b * (255 - a) + 127) / 255; };
                        image[i] = mix(p & 0xFF, back[0]) | (mix((p >> 8) & 0xFF, back[1]) << 8) | (mix((p >> 16) & 0xFF, back[2]) << 16) | (255u << 24);
                    }
                    char name[96];
                    snprintf(name, sizeof(name), "%s_%dx%d_%d.png", language.id, size.w, size.h, shot);
                    if (gpu::WritePng(directory / name, reinterpret_cast<const uint8_t*>(image.data()), uint32_t(size.w), uint32_t(size.h),
                                      uint32_t(size.w) * 4))
                        written++;
                }
            }
        }
        g_texts = nullptr;
        g_drawn = {};
        printf("cheats preview: %d images in %s\n", written, directory.string().c_str());
        return written ? 0 : 1;
    }
}
