// The developer console (dev_console.h; README "Console de desenvolvedor", docs/dev_features.md).
//
// Three threads touch it: the window thread types into it (OnKeyDown / OnChar / OnWheel), the
// game thread runs the commands (RunPending, from the script update hook in dev_tools.cpp, where
// the game's Lua can be called) and the render thread draws it (Draw: the text is rasterized with
// GDI, Consolas, into a CPU bitmap redrawn only when something changes, and drawn by the overlay
// over the game image like the save screens; screenshots include it). Everything shared is under
// g_mutex.

#include "dev_console.h"
#include "dev_tools.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../gpu/window.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../paths.h"
#include "../settings/direct_boot.h"
#include "../settings/settings.h"
#include "../settings/ui_text.h"
#include "../platform/platform.h"

#include "../platform/keys.h"
#include "../platform/text.h"
#include "../platform/text_canvas.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PPC_EXTERN_FUNC(sub_820F5D58);  // Frontend:StartLevel(index)

namespace dev::console
{
    namespace
    {
        constexpr size_t MAX_LINES = 400;
        constexpr size_t MAX_HISTORY = 200;
        constexpr uint64_t RELEASE_MS = 250;  // the game still sees a neutral pad after closing
        constexpr uint32_t TOGGLE_SCAN = 0x29;     // the key left of 1 (` on US, ' on ABNT2)

        struct Line
        {
            std::string text;
            Tone tone;
        };

        std::mutex g_mutex;
        std::deque<Line> g_lines;
        uint64_t g_generation = 1;  // bumped by every visible change
        std::string g_input;
        size_t g_caret = 0;  // byte offset in g_input
        std::vector<std::string> g_history;
        int g_historyIndex = -1;  // -1: editing a new line
        std::string g_draft;      // the new line while walking the history
        bool g_historyLoaded = false;
        int g_scroll = 0;  // lines scrolled up from the newest
        std::deque<std::string> g_queue;  // commands for the game thread
        bool g_swallowChar = false;        // the WM_CHAR of the toggle key
        std::vector<std::string> g_levels;  // the game's level list (game thread fills it)
        std::string g_lastTraceback;        // the last "erro: ..." of _TRACEBACK (not repeated)
        // Tab completion cycling.
        std::vector<std::string> g_matches;
        size_t g_matchIndex = 0;
        size_t g_tokenStart = 0;
        bool g_cycling = false;

        std::atomic<int32_t> g_levelRequest{ -1 };  // "level" outside a level: the index, for OverrideExit
        constexpr uint32_t NODE_MAIN_MENU = 70, NODE_HUB = 575;
        constexpr int32_t EXIT_SKIP_TO_LEVEL = 51;  // the developers' SkipToLevel (-> 630 InGame)
        std::atomic<bool> g_open{ false };
        std::atomic<uint64_t> g_closedAt{ 0 };
        bool g_argument = false;

        // ---- text helpers ----
        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(tolower(uint8_t(c)));
            return text;
        }

        std::string Trim(const std::string& text)
        {
            size_t a = text.find_first_not_of(" \t\r\n");
            if (a == std::string::npos)
                return {};
            size_t b = text.find_last_not_of(" \t\r\n");
            return text.substr(a, b - a + 1);
        }

        std::vector<std::string> Split(const std::string& text)
        {
            std::vector<std::string> words;
            std::istringstream in(text);
            std::string word;
            while (in >> word)
                words.push_back(word);
            return words;
        }

        std::wstring Wide(const std::string& utf8)
        {
            return plat::WidenUtf8(utf8);
        }

        std::string Utf8(const std::wstring& wide)
        {
            return plat::NarrowUtf8(wide);
        }

        // Byte offsets of the UTF-8 character before / after `at`.
        size_t PrevChar(const std::string& text, size_t at)
        {
            if (at == 0)
                return 0;
            at--;
            while (at > 0 && (uint8_t(text[at]) & 0xC0) == 0x80)
                at--;
            return at;
        }

        size_t NextChar(const std::string& text, size_t at)
        {
            if (at >= text.size())
                return text.size();
            at++;
            while (at < text.size() && (uint8_t(text[at]) & 0xC0) == 0x80)
                at++;
            return at;
        }

        bool ParseNumber(const std::string& text, double& value)
        {
            char* end = nullptr;
            value = strtod(text.c_str(), &end);
            return !text.empty() && end && !*end;
        }

        // ---- output (callers hold g_mutex) ----
        void AddLine(const std::string& text, Tone tone)
        {
            size_t start = 0;
            while (true)
            {
                size_t end = text.find('\n', start);
                g_lines.push_back({ text.substr(start, end == std::string::npos ? std::string::npos : end - start), tone });
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            while (g_lines.size() > MAX_LINES)
                g_lines.pop_front();
            if (g_scroll > 0)
                g_scroll = std::min<int>(g_scroll + 1, int(g_lines.size()) - 1);
            g_generation++;
        }

        // ---- history ----
        std::filesystem::path HistoryPath() { return paths::UserData() / "console_history.txt"; }

        void LoadHistory()
        {
            if (g_historyLoaded)
                return;
            g_historyLoaded = true;
            std::ifstream in(HistoryPath(), std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    g_history.push_back(line);
            }
            if (g_history.size() > MAX_HISTORY)
                g_history.erase(g_history.begin(), g_history.end() - MAX_HISTORY);
        }

        void SaveHistory()
        {
            std::ofstream out(HistoryPath(), std::ios::binary | std::ios::trunc);
            for (const std::string& line : g_history)
                out << line << "\n";
        }

        // ---- texts (settings/ui_text.h: game.language; the command names are the same in all) ----
        using settings::Tr;
        using settings::UiText;
        const UiText T_COMMANDS = { "Commands:", "Comandos:", "Commandes :", "Befehle:", "Comandos:", "Comandi:", "Opdrachten:" };
        const UiText T_UNKNOWN_COMMAND = { "unknown command: ", "comando desconhecido: ", "commande inconnue : ", "unbekannter Befehl: ",
                                           "comando desconocido: ", "comando sconosciuto: ", "onbekende opdracht: " };
        // " (\"help\" lists them)": %s is the command that lists them.
        const UiText T_LISTED_BY = { " (\"%s\" lists them)", " (\"%s\" lista)", " (\"%s\" les liste)", " (\"%s\" listet sie auf)",
                                     " (\"%s\" los lista)", " (\"%s\" li elenca)", " (\"%s\" toont ze)" };
        const UiText T_USAGE = { "usage: ", "uso: ", "usage : ", "Verwendung: ", "uso: ", "uso: ", "gebruik: " };
        const UiText T_ERROR = { "error: ", "erro: ", "erreur : ", "Fehler: ", "error: ", "errore: ", "fout: " };
        const UiText T_NO_LUA = { "the game's Lua state doesn't exist yet", "o estado Lua do jogo ainda não existe",
                                  "l'état Lua du jeu n'existe pas encore", "der Lua-Zustand des Spiels existiert noch nicht",
                                  "el estado Lua del juego aún no existe", "lo stato Lua del gioco non esiste ancora",
                                  "de Lua-toestand van het spel bestaat nog niet" };
        const UiText T_UNUSABLE = { "  ((x) = can't be used)", "  ((x) = não dá para usar)", "  ((x) = inutilisable)", "  ((x) = nicht nutzbar)",
                                    "  ((x) = no se puede usar)", "  ((x) = non utilizzabile)", "  ((x) = niet bruikbaar)" };
        const UiText T_LEVELS = { "levels: ", "fases: ", "niveaux : ", "Level: ", "niveles: ", "livelli: ", "levels: " };
        const UiText T_NO_LEVELS = { "the level list doesn't exist yet", "a lista de fases ainda não existe",
                                     "la liste des niveaux n'existe pas encore", "die Levelliste existiert noch nicht",
                                     "la lista de niveles aún no existe", "l'elenco dei livelli non esiste ancora", "de levellijst bestaat nog niet" };
        const UiText T_UNKNOWN_LEVEL = { "unknown level: ", "fase desconhecida: ", "niveau inconnu : ", "unbekanntes Level: ",
                                         "nivel desconocido: ", "livello sconosciuto: ", "onbekend level: " };
        const UiText T_GOING_TO = { "going to ", "indo para ", "direction ", "gehe zu ", "yendo a ", "vado a ", "naar " };
        const UiText T_FROM_MENUS = { " (through the HUB or the main menu)", " (pelo HUB ou pelo menu principal)", " (par le HUB ou le menu principal)",
                                      " (über den HUB oder das Hauptmenü)", " (por el HUB o el menú principal)", " (dall'HUB o dal menu principale)",
                                      " (via de HUB of het hoofdmenu)" };
        const UiText T_RELOADING = { "restarting the level", "reiniciando a fase", "relance du niveau", "Level wird neu gestartet",
                                     "reiniciando el nivel", "riavvio del livello", "level wordt herstart" };
        const UiText T_CURRENT_SKIN = { "current skin: ", "skin atual: ", "skin actuel : ", "aktueller Skin: ", "skin actual: ", "skin attuale: ",
                                        "huidige skin: " };
        const UiText T_NEXT_LEVEL = { " (next level: ", " (próxima fase: ", " (prochain niveau : ", " (nächstes Level: ", " (próximo nivel: ",
                                      " (prossimo livello: ", " (volgend level: " };
        const UiText T_UNKNOWN_SKIN = { "unknown skin: ", "skin desconhecida: ", "skin inconnu : ", "unbekannter Skin: ", "skin desconocido: ",
                                        "skin sconosciuta: ", "onbekende skin: " };
        const UiText T_REFUSED = { "refused: ", "recusado: ", "refusé : ", "abgelehnt: ", "rechazado: ", "rifiutato: ", "geweigerd: " };
        const UiText T_WELCOME = { "CrashRecomp - developer console. \"help\" lists the commands.",
                                   "CrashRecomp - console de desenvolvedor. \"help\" lista os comandos.",
                                   "CrashRecomp - console de développement. \"help\" liste les commandes.",
                                   "CrashRecomp - Entwicklerkonsole. \"help\" listet die Befehle auf.",
                                   "CrashRecomp - consola de desarrollador. \"help\" lista los comandos.",
                                   "CrashRecomp - console sviluppatore. \"help\" elenca i comandi.",
                                   "CrashRecomp - ontwikkelaarsconsole. \"help\" toont de opdrachten." };
        const UiText T_KEYS = { "Tab completes, Up/Down arrows = history, PageUp/PageDown scroll, Esc closes.",
                                "Tab completa, setas para cima/baixo = histórico, PageUp/PageDown rolam, Esc fecha.",
                                "Tab complète, flèches haut/bas = historique, PageUp/PageDown font défiler, Esc ferme.",
                                "Tab ergänzt, Pfeil hoch/runter = Verlauf, PageUp/PageDown blättern, Esc schließt.",
                                "Tab completa, flechas arriba/abajo = historial, PageUp/PageDown desplazan, Esc cierra.",
                                "Tab completa, frecce su/giù = cronologia, PageUp/PageDown scorrono, Esc chiude.",
                                "Tab vult aan, pijlen omhoog/omlaag = geschiedenis, PageUp/PageDown scrollen, Esc sluit." };
        const UiText T_HEADER = { "DEVELOPER CONSOLE  -  help: commands   Tab: complete   ↑↓: history   Esc/F10: close",
                                  "CONSOLE DE DESENVOLVEDOR  -  help: comandos   Tab: completar   ↑↓: histórico   Esc/F10: fechar",
                                  "CONSOLE DE DÉVELOPPEMENT  -  help : commandes   Tab : compléter   ↑↓ : historique   Esc/F10 : fermer",
                                  "ENTWICKLERKONSOLE  -  help: Befehle   Tab: ergänzen   ↑↓: Verlauf   Esc/F10: schließen",
                                  "CONSOLA DE DESARROLLADOR  -  help: comandos   Tab: completar   ↑↓: historial   Esc/F10: cerrar",
                                  "CONSOLE SVILUPPATORE  -  help: comandi   Tab: completa   ↑↓: cronologia   Esc/F10: chiudi",
                                  "ONTWIKKELAARSCONSOLE  -  help: opdrachten   Tab: aanvullen   ↑↓: geschiedenis   Esc/F10: sluiten" };
        // "[+12 lines]": the output is scrolled up that many lines.
        const UiText T_SCROLLED = { "lines", "linhas", "lignes", "Zeilen", "líneas", "righe", "regels" };

        // `text` with its "%s" replaced by `value`.
        std::string Fill(const char* text, const std::string& value)
        {
            std::string out = text;
            if (size_t at = out.find("%s"); at != std::string::npos)
                out.replace(at, 2, value);
            return out;
        }

        // ---- commands ----
        struct Command
        {
            const char* name;
            UiText usage;
            UiText help;
        };
        const Command COMMANDS[] = {
            { "help",
              { "help [command]", "help [comando]", "help [commande]", "help [befehl]", "help [comando]", "help [comando]", "help [opdracht]" },
              { "lists the commands, or explains one", "lista os comandos, ou explica um", "liste les commandes, ou en explique une",
                "listet die Befehle auf oder erklärt einen", "lista los comandos o explica uno", "elenca i comandi o ne spiega uno",
                "toont de opdrachten, of legt er een uit" } },
            { "unlock",
              { "unlock all | unlock levels | unlock skin <name>", "unlock all | unlock levels | unlock skin <nome>",
                "unlock all | unlock levels | unlock skin <nom>", "unlock all | unlock levels | unlock skin <name>",
                "unlock all | unlock levels | unlock skin <nombre>", "unlock all | unlock levels | unlock skin <nome>",
                "unlock all | unlock levels | unlock skin <naam>" },
              { "unlocks everything (20 skins + Crash_Hulk, levels, moves), only the levels (the skins stay locked) or one skin",
                "desbloqueia tudo (20 skins + Crash_Hulk, fases, golpes), só as fases (as skins ficam trancadas) ou uma skin",
                "débloque tout (20 skins + Crash_Hulk, niveaux, coups), seulement les niveaux (les skins restent verrouillés) ou un skin",
                "schaltet alles frei (20 Skins + Crash_Hulk, Level, Angriffe), nur die Level (die Skins bleiben gesperrt) oder einen Skin",
                "desbloquea todo (20 skins + Crash_Hulk, niveles, golpes), solo los niveles (los skins siguen bloqueados) o un skin",
                "sblocca tutto (20 skin + Crash_Hulk, livelli, mosse), solo i livelli (le skin restano bloccate) o una skin",
                "ontgrendelt alles (20 skins + Crash_Hulk, levels, aanvallen), alleen de levels (de skins blijven op slot) of één skin" } },
            { "mojo", { "mojo <n>", "mojo <n>", "mojo <n>", "mojo <n>", "mojo <n>", "mojo <n>", "mojo <n>" },
              { "gives n mojo (the HUD follows)", "dá n de mojo (o HUD acompanha)", "donne n de mojo (le HUD suit)", "gibt n Mojo (das HUD zieht mit)",
                "da n de mojo (el HUD se actualiza)", "dà n di mojo (l'HUD si aggiorna)", "geeft n mojo (de HUD volgt)" } },
            { "god", { "god [on|off]", "god [on|off]", "god [on|off]", "god [on|off]", "god [on|off]", "god [on|off]", "god [on|off]" },
              { "invincibility (HP doesn't go down; pits still kill)", "invencibilidade (o HP não desce; poços ainda matam)",
                "invincibilité (les PV ne baissent pas ; les gouffres tuent toujours)", "Unverwundbarkeit (die HP sinken nicht; Abgründe töten trotzdem)",
                "invencibilidad (la vida no baja; los pozos siguen matando)", "invincibilità (gli HP non scendono; i burroni uccidono comunque)",
                "onkwetsbaarheid (de HP daalt niet; afgronden doden nog steeds)" } },
            { "tp", { "tp <x> <y> <z>", "tp <x> <y> <z>", "tp <x> <y> <z>", "tp <x> <y> <z>", "tp <x> <y> <z>", "tp <x> <y> <z>", "tp <x> <y> <z>" },
              { "teleports player 1 (y = height; see pos)", "teleporta o jogador 1 (y = altura; veja pos)",
                "téléporte le joueur 1 (y = hauteur ; voir pos)", "teleportiert Spieler 1 (y = Höhe; siehe pos)",
                "teletransporta al jugador 1 (y = altura; ver pos)", "teletrasporta il giocatore 1 (y = altezza; vedi pos)",
                "teleporteert speler 1 (y = hoogte; zie pos)" } },
            { "pos", { "pos", "pos", "pos", "pos", "pos", "pos", "pos" },
              { "shows player 1's position", "mostra a posição do jogador 1", "affiche la position du joueur 1", "zeigt die Position von Spieler 1",
                "muestra la posición del jugador 1", "mostra la posizione del giocatore 1", "toont de positie van speler 1" } },
            { "shadow",
              { "shadow [on|off]", "shadow [on|off]", "shadow [on|off]", "shadow [on|off]", "shadow [on|off]", "shadow [on|off]", "shadow [on|off]" },
              { "the game's Shadow Crash cheat", "cheat Shadow Crash do jogo", "le code Shadow Crash du jeu", "der Shadow-Crash-Cheat des Spiels",
                "el truco Shadow Crash del juego", "il trucco Shadow Crash del gioco", "de Shadow Crash-cheat van het spel" } },
            { "bighead",
              { "bighead [on|off]", "bighead [on|off]", "bighead [on|off]", "bighead [on|off]", "bighead [on|off]", "bighead [on|off]", "bighead [on|off]" },
              { "the game's Big Head cheat", "cheat Big Head do jogo", "le code Big Head du jeu", "der Big-Head-Cheat des Spiels",
                "el truco Big Head del juego", "il trucco Big Head del gioco", "de Big Head-cheat van het spel" } },
            { "skin",
              { "skin [<name> [now]]", "skin [<nome> [now]]", "skin [<nom> [now]]", "skin [<name> [now]]", "skin [<nombre> [now]]", "skin [<nome> [now]]",
                "skin [<naam> [now]]" },
              { "puts on a skin (in a level: at the next load; now restarts the level); without a name, lists them",
                "veste uma skin (na fase: na próxima carga; now reinicia a fase); sem nome lista",
                "met un skin (dans un niveau : au prochain chargement ; now relance le niveau) ; sans nom, les liste",
                "zieht einen Skin an (im Level: beim nächsten Laden; now startet das Level neu); ohne Namen: Liste",
                "pone un skin (en un nivel: en la próxima carga; now reinicia el nivel); sin nombre, los lista",
                "indossa una skin (in un livello: al prossimo caricamento; now riavvia il livello); senza nome le elenca",
                "trekt een skin aan (in een level: bij het volgende laden; now herstart het level); zonder naam: lijst" } },
            { "level",
              { "level [<name>]", "level [<nome>]", "level [<nom>]", "level [<name>]", "level [<nombre>]", "level [<nome>]", "level [<naam>]" },
              { "goes to the level (from inside a level, the HUB or the main menu); without a name, lists them",
                "vai para a fase (de dentro de uma fase, do HUB ou do menu principal); sem nome lista",
                "va au niveau (depuis un niveau, le HUB ou le menu principal) ; sans nom, les liste",
                "geht zum Level (aus einem Level, dem HUB oder dem Hauptmenü); ohne Namen: Liste",
                "va al nivel (desde un nivel, el HUB o el menú principal); sin nombre, los lista",
                "va al livello (da un livello, dall'HUB o dal menu principale); senza nome li elenca",
                "gaat naar het level (vanuit een level, de HUB of het hoofdmenu); zonder naam: lijst" } },
            { "reload", { "reload", "reload", "reload", "reload", "reload", "reload", "reload" },
              { "restarts the current level (UberManager:ForceReload)", "reinicia a fase atual (UberManager:ForceReload)",
                "relance le niveau actuel (UberManager:ForceReload)", "startet das aktuelle Level neu (UberManager:ForceReload)",
                "reinicia el nivel actual (UberManager:ForceReload)", "riavvia il livello attuale (UberManager:ForceReload)",
                "herstart het huidige level (UberManager:ForceReload)" } },
            { "lua",
              { "lua <code> | lua =<expression>", "lua <código> | lua =<expressão>", "lua <code> | lua =<expression>", "lua <code> | lua =<ausdruck>",
                "lua <código> | lua =<expresión>", "lua <codice> | lua =<espressione>", "lua <code> | lua =<expressie>" },
              { "runs Lua in the game's state (devprint writes here)", "roda Lua no estado do jogo (devprint escreve aqui)",
                "exécute du Lua dans l'état du jeu (devprint écrit ici)", "führt Lua im Zustand des Spiels aus (devprint schreibt hierher)",
                "ejecuta Lua en el estado del juego (devprint escribe aquí)", "esegue Lua nello stato del gioco (devprint scrive qui)",
                "voert Lua uit in de toestand van het spel (devprint schrijft hier)" } },
            { "clear", { "clear", "clear", "clear", "clear", "clear", "clear", "clear" },
              { "clears the output (Ctrl+L)", "limpa a saída (Ctrl+L)", "efface la sortie (Ctrl+L)", "leert die Ausgabe (Strg+L)",
                "borra la salida (Ctrl+L)", "pulisce l'output (Ctrl+L)", "wist de uitvoer (Ctrl+L)" } },
        };

        std::vector<std::string> CommandNames()
        {
            std::vector<std::string> names;
            for (const Command& c : COMMANDS)
                names.push_back(c.name);
            return names;
        }

        // Candidates for the word being typed (index 0 = the command).
        std::vector<std::string> Candidates(const std::vector<std::string>& words, size_t index)
        {
            if (index == 0)
                return CommandNames();
            std::string command = Lower(words[0]);
            if (command == "unlock")
                return index == 1 ? std::vector<std::string>{ "all", "levels", "skin" }
                       : index == 2 && words.size() > 1 && Lower(words[1]) == "skin" ? skins::Names()
                                                                                      : std::vector<std::string>{};
            if (command == "skin")
                return index == 1 ? skins::Names() : index == 2 ? std::vector<std::string>{ "now" } : std::vector<std::string>{};
            if (command == "god" || command == "shadow" || command == "bighead")
                return index == 1 ? std::vector<std::string>{ "on", "off" } : std::vector<std::string>{};
            if (command == "level")
                return index == 1 ? g_levels : std::vector<std::string>{};
            if (command == "help")
                return index == 1 ? CommandNames() : std::vector<std::string>{};
            return {};
        }

        // Tab (window thread, g_mutex held).
        void Complete()
        {
            if (g_cycling && !g_matches.empty())
            {
                g_matchIndex = (g_matchIndex + 1) % g_matches.size();
                g_input = g_input.substr(0, g_tokenStart) + g_matches[g_matchIndex];
                g_caret = g_input.size();
                g_generation++;
                return;
            }
            std::string before = g_input.substr(0, g_caret);
            size_t tokenStart = before.find_last_of(' ');
            tokenStart = tokenStart == std::string::npos ? 0 : tokenStart + 1;
            std::vector<std::string> words = Split(before.substr(0, tokenStart));
            std::string prefix = Lower(before.substr(tokenStart));
            if (Lower(words.empty() ? "" : words[0]) == "lua")
                return;
            std::vector<std::string> matches;
            for (const std::string& candidate : Candidates(words, words.size()))
                if (Lower(candidate).rfind(prefix, 0) == 0)
                    matches.push_back(candidate);
            if (matches.empty())
                return;
            if (matches.size() == 1)
            {
                g_input = g_input.substr(0, tokenStart) + matches[0] + " " + g_input.substr(g_caret);
                g_caret = tokenStart + matches[0].size() + 1;
                g_generation++;
                return;
            }
            // Several: the common prefix now, the list in the output; Tab again cycles them.
            std::string common = matches[0];
            for (const std::string& m : matches)
            {
                size_t n = 0;
                while (n < common.size() && n < m.size() && tolower(uint8_t(common[n])) == tolower(uint8_t(m[n])))
                    n++;
                common.resize(n);
            }
            std::string list;
            for (const std::string& m : matches)
                list += (list.empty() ? "" : "  ") + m;
            AddLine(list, Tone::Dim);
            g_input = g_input.substr(0, tokenStart) + (common.size() > prefix.size() ? common : before.substr(tokenStart));
            g_caret = g_input.size();
            g_matches = matches;
            g_matchIndex = size_t(-1);
            g_tokenStart = tokenStart;
            g_cycling = true;
        }

        void Submit()
        {
            std::string line = Trim(g_input);
            g_input.clear();
            g_caret = 0;
            g_historyIndex = -1;
            g_scroll = 0;
            if (line.empty())
            {
                g_generation++;
                return;
            }
            LoadHistory();
            if (g_history.empty() || g_history.back() != line)
            {
                g_history.push_back(line);
                if (g_history.size() > MAX_HISTORY)
                    g_history.erase(g_history.begin());
                SaveHistory();
            }
            AddLine("> " + line, Tone::Input);
            g_queue.push_back(line);
            Log("[console] > %s", line.c_str());
        }

        void HistoryStep(int direction)
        {
            LoadHistory();
            if (g_history.empty())
                return;
            if (g_historyIndex == -1)
            {
                if (direction > 0)
                    return;
                g_draft = g_input;
                g_historyIndex = int(g_history.size()) - 1;
            }
            else
            {
                g_historyIndex += direction;
                if (g_historyIndex < 0)
                    g_historyIndex = 0;
                if (g_historyIndex >= int(g_history.size()))
                {
                    g_historyIndex = -1;
                    g_input = g_draft;
                    g_caret = g_input.size();
                    g_generation++;
                    return;
                }
            }
            g_input = g_history[size_t(g_historyIndex)];
            g_caret = g_input.size();
            g_generation++;
        }

        void Open()
        {
            {
                std::lock_guard lock(g_mutex);
                LoadHistory();
                if (g_lines.empty())
                {
                    AddLine(Tr(T_WELCOME), Tone::Help);
                    AddLine(Tr(T_KEYS), Tone::Dim);
                }
                g_generation++;
            }
            g_open = true;
            Log("[console] opened");
            boot::Event("console_open");
        }

        void Close()
        {
            g_open = false;
            g_closedAt = plat::TickCountMs();
            Log("[console] closed");
            boot::Event("console_close");
        }

        // ---- game thread ----
        void Reply(const std::string& text, Tone tone = Tone::Normal)
        {
            Print(text, tone);
        }

        // Lua for a command; errors in red (the _TRACEBACK line already says it for runtime errors).
        bool ConsoleLua(PPCContext& ctx, uint8_t* base, const std::string& code)
        {
            {
                std::lock_guard lock(g_mutex);
                g_lastTraceback.clear();
            }
            std::string error;
            bool ok = RunLua(ctx, base, code.c_str(), "console", &error);
            if (!ok)
            {
                std::lock_guard lock(g_mutex);
                if (error.empty())
                    AddLine(std::string(Tr(T_ERROR)) + Tr(T_NO_LUA), Tone::Error);
                else if (error != g_lastTraceback)
                    AddLine(Tr(T_ERROR) + error, Tone::Error);
            }
            return ok;
        }

        // "on" / "off" / nothing (toggle from `current`); -1 = bad word.
        int OnOff(const std::vector<std::string>& words, bool current)
        {
            if (words.size() < 2)
                return current ? 0 : 1;
            std::string w = Lower(words[1]);
            if (w == "on" || w == "1" || w == "true" || w == "liga")
                return 1;
            if (w == "off" || w == "0" || w == "false" || w == "desliga")
                return 0;
            return -1;
        }

        std::string LuaNumber(double value)
        {
            char text[64];
            snprintf(text, sizeof(text), "%.4f", value);
            return text;
        }

        void Help(const std::string& topic)
        {
            if (!topic.empty())
            {
                for (const Command& c : COMMANDS)
                {
                    if (Lower(topic) == c.name)
                    {
                        Reply(Tr(c.usage), Tone::Help);
                        Reply("  " + std::string(Tr(c.help)), Tone::Normal);
                        if (std::string(c.name) == "skin" || std::string(c.name) == "unlock")
                            Reply("  skins: " + skins::List() + Tr(T_UNUSABLE), Tone::Dim);
                        if (std::string(c.name) == "level" && !g_levels.empty())
                        {
                            std::string list;
                            for (const std::string& l : g_levels)
                                list += (list.empty() ? "" : " ") + l;
                            Reply("  " + std::string(Tr(T_LEVELS)) + list, Tone::Dim);
                        }
                        return;
                    }
                }
                Reply(Tr(T_UNKNOWN_COMMAND) + topic, Tone::Error);
                return;
            }
            Reply(Tr(T_COMMANDS), Tone::Help);
            for (const Command& c : COMMANDS)
            {
                // Padded to 34 characters (not bytes: the usages have accented letters).
                std::string usage = Tr(c.usage);
                usage.append(size_t(std::max(34 - int(Wide(usage).size()), 0)), ' ');
                Reply("  " + usage + Tr(c.help), Tone::Normal);
            }
        }

        void Execute(PPCContext& ctx, uint8_t* base, const std::string& line)
        {
            std::vector<std::string> words = Split(line);
            if (words.empty())
                return;
            std::string command = Lower(words[0]);
            auto usage = [&](const char* name) {
                for (const Command& c : COMMANDS)
                    if (std::string(c.name) == name)
                        Reply(std::string(Tr(T_USAGE)) + Tr(c.usage), Tone::Error);
            };

            if (command == "help" || command == "?")
                Help(words.size() > 1 ? words[1] : "");
            else if (command == "clear" || command == "cls")
            {
                std::lock_guard lock(g_mutex);
                g_lines.clear();
                g_scroll = 0;
                g_generation++;
            }
            else if (command == "unlock")
            {
                std::string what = words.size() > 1 ? Lower(words[1]) : "";
                if (what == "all")
                {
                    if (ConsoleLua(ctx, base, "dev.unlock_all()"))
                    {
                        skins::UnlockHulk(true);
                        Reply("unlock all: 20 skins do jogo, fases e golpes (objetivos do jogo) + Crash_Hulk no menu de skins", Tone::Ok);
                        Reply("  (o jogo grava no save quando ele salvar)", Tone::Dim);
                    }
                }
                else if (what == "levels")
                {
                    if (ConsoleLua(ctx, base, "dev.unlock_levels()"))
                    {
                        Reply("unlock levels: as fases e episodios (objetivo do jogo); skins e golpes continuam como estao", Tone::Ok);
                        Reply("  (o jogo grava no save quando ele salvar)", Tone::Dim);
                    }
                }
                else if (what == "skin" && words.size() > 2)
                {
                    int skin = skins::ByName(words[2]);
                    if (skin < 0)
                        Reply("skin desconhecida: " + words[2] + " (skins: " + skins::List() + ")", Tone::Error);
                    else if (!skins::Wearable(skin))
                        Reply(std::string("recusado: ") + skins::Label(skin), Tone::Error);
                    else if (skin == skins::HULK)
                    {
                        skins::UnlockHulk(true);
                        Reply("Crash_Hulk desbloqueada no menu de skins (sem ocupar vaga da lista do jogo; [dev] hulk_skin)", Tone::Ok);
                    }
                    else if (skin == skins::CRASH)
                        Reply("o Crash normal ja vem desbloqueado", Tone::Normal);
                    else if (ConsoleLua(ctx, base, "dev.unlock_skin(" + std::to_string(skin) + ")"))
                        Reply(std::string("skin ") + skins::Label(skin) + " desbloqueada (lista do jogo, 20 vagas)", Tone::Ok);
                }
                else
                    usage("unlock");
            }
            else if (command == "mojo")
            {
                double amount;
                if (words.size() < 2 || !ParseNumber(words[1], amount) || amount != double(int64_t(amount)) || amount < -100000 || amount > 100000)
                    usage("mojo");
                else
                    ConsoleLua(ctx, base, "dev.give_mojo(" + std::to_string(int64_t(amount)) + ")");
            }
            else if (command == "god")
            {
                int on = OnOff(words, Invincible());
                if (on < 0)
                    usage("god");
                else
                {
                    SetInvincible(on != 0);
                    Reply(on ? "god: ligado (invencivel)" : "god: desligado", Tone::Ok);
                }
            }
            else if (command == "tp" || command == "teleport")
            {
                double x, y, z;
                if (words.size() < 4 || !ParseNumber(words[1], x) || !ParseNumber(words[2], y) || !ParseNumber(words[3], z))
                    usage("tp");
                else
                    ConsoleLua(ctx, base, "dev.teleport(" + LuaNumber(x) + ", " + LuaNumber(y) + ", " + LuaNumber(z) + ")");
            }
            else if (command == "pos")
                ConsoleLua(ctx, base, "dev.pos()");
            else if (command == "shadow")
            {
                int on = OnOff(words, false);
                if (words.size() < 2)
                    on = -2;  // toggle in Lua
                if (on == -1)
                    usage("shadow");
                else
                    ConsoleLua(ctx, base, on == -2 ? "dev.setting('m_CheatShadowCrash', not getobj(':SettingsDatabase').m_CheatShadowCrash)"
                                                   : std::string("dev.setting('m_CheatShadowCrash', ") + (on ? "true" : "false") + ")");
            }
            else if (command == "bighead")
            {
                static bool bigHead = false;
                int on = OnOff(words, bigHead);
                if (on < 0)
                    usage("bighead");
                else
                {
                    bigHead = on != 0;
                    int players = SetBigHead(ctx, base, bigHead);
                    Reply(std::string("bighead: ") + (bigHead ? "ligado" : "desligado") + (players ? "" : " (nenhum jogador agora: vale na fase)"), Tone::Ok);
                }
            }
            else if (command == "skin")
            {
                if (words.size() < 2)
                {
                    int current = skins::Current(ctx, base), pending = skins::Pending();
                    Reply(std::string(Tr(T_CURRENT_SKIN)) + (current >= 0 ? skins::Label(current) : "?") +
                              (pending >= 0 ? std::string(Tr(T_NEXT_LEVEL)) + skins::Label(pending) + ")" : ""),
                          Tone::Help);
                    Reply("skins: " + skins::List() + Tr(T_UNUSABLE), Tone::Dim);
                    return;
                }
                int skin = skins::ByName(words[1]);
                bool now = words.size() > 2 && Lower(words[2]) == "now";
                if (skin < 0)
                    Reply(Tr(T_UNKNOWN_SKIN) + words[1] + " (skins: " + skins::List() + ")", Tone::Error);
                else if (!skins::Wearable(skin))
                    Reply(std::string(Tr(T_REFUSED)) + skins::Label(skin), Tone::Error);
                else
                    Reply(skins::Select(ctx, base, skin, now), Tone::Ok);
            }
            else if (command == "level")
            {
                if (g_levels.empty())
                    g_levels = Levels();
                if (words.size() < 2)
                {
                    std::string list;
                    for (const std::string& l : g_levels)
                        list += (list.empty() ? "" : " ") + l;
                    Reply(list.empty() ? std::string(Tr(T_NO_LEVELS)) : Tr(T_LEVELS) + list, Tone::Help);
                    return;
                }
                std::string name;
                for (const std::string& l : g_levels)
                    if (plat::CompareNoCase(l.c_str(), words[1].c_str()) == 0)
                        name = l;
                if (name.empty())
                {
                    Reply(Tr(T_UNKNOWN_LEVEL) + words[1] + Fill(Tr(T_LISTED_BY), "level"), Tone::Error);
                    return;
                }
                if (skins::InLevel())
                {
                    // In a level, Frontend:StartLevelByName (game state 5) leaves the game waiting for
                    // the frontend to start the level, which it only does from its menus: the game's
                    // own restart (UberManager:ForceReload) with the new index loads it instead.
                    size_t index = size_t(std::find(g_levels.begin(), g_levels.end(), name) - g_levels.begin());
                    Log("[console] level %s: UberManager SetLevelIndex(%zu) + ForceReload", name.c_str(), index);
                    if (ConsoleLua(ctx, base, "local u = getobj(':UberManager') u:SetLevelIndex(" + std::to_string(index) + ") u:ForceReload()"))
                        Reply(Tr(T_GOING_TO) + name, Tone::Ok);
                    return;
                }
                // In the frontend the level starts at the next evaluation of the HUB or the main
                // menu (OverrideExit): Frontend:StartLevel alone doesn't leave the menu.
                g_levelRequest = int32_t(std::find(g_levels.begin(), g_levels.end(), name) - g_levels.begin());
                Log("[console] level %s: requested from the frontend", name.c_str());
                Reply(Tr(T_GOING_TO) + name + Tr(T_FROM_MENUS), Tone::Ok);
            }
            else if (command == "reload")
            {
                if (ConsoleLua(ctx, base, "Managers.UberManager:ForceReload()"))
                    Reply(Tr(T_RELOADING), Tone::Ok);
            }
            else if (command == "lua")
            {
                size_t at = line.find_first_not_of(" \t", line.find_first_of(" \t"));
                std::string code = at == std::string::npos ? "" : Trim(line.substr(at));
                if (code.empty())
                    usage("lua");
                else if (code[0] == '=')
                    ConsoleLua(ctx, base, "devprint(tostring(" + code.substr(1) + "))");
                else
                    ConsoleLua(ctx, base, code);
            }
            else
                Reply(Tr(T_UNKNOWN_COMMAND) + words[0] + Fill(Tr(T_LISTED_BY), "help"), Tone::Error);
        }

        // ---- the test script (CRASH_DEV_CONSOLE_SCRIPT) ----
        struct ScriptEntry
        {
            boot::Anchored at;
            std::string text;
            bool done = false;
        };
        std::vector<ScriptEntry> g_script;

        void LoadScript()
        {
            const char* path = getenv("CRASH_DEV_CONSOLE_SCRIPT");
            if (!path || !*path)
                return;
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                Log("[console] can't read CRASH_DEV_CONSOLE_SCRIPT %s", path);
                return;
            }
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                size_t bar = line.find('|');
                if (line.empty() || line[0] == '#' || bar == std::string::npos)
                    continue;
                ScriptEntry entry;
                std::string time = Trim(line.substr(0, bar));
                const char* p = time.c_str();
                if (!boot::ParseAnchored(p, 1000.0, entry.at))
                {
                    Log("[console] script: bad time \"%s\"", time.c_str());
                    continue;
                }
                entry.text = line.substr(bar + 1);
                g_script.push_back(entry);
            }
            Log("[console] script: %zu entries from %s", g_script.size(), path);
        }

        void RunScriptEntry(const std::string& text)
        {
            using gpu::window::PostKey;
            if (!gpu::window::Handle())
                return;
            if (text == "!toggle")
                PostKey(0, TOGGLE_SCAN, false);
            else if (text == "!esc")
                PostKey(VK_ESCAPE, 0x01, false);
            else if (text == "!tab")
                PostKey(VK_TAB, 0x0F, false);
            else if (text == "!up")
                PostKey(VK_UP, 0x48, true);
            else if (text == "!down")
                PostKey(VK_DOWN, 0x50, true);
            else if (text == "!enter")
                PostKey(VK_RETURN, 0x1C, false);
            else
            {
                bool enter = text.rfind("!type:", 0) != 0;
                for (wchar_t c : Wide(enter ? text : text.substr(6)))
                    gpu::window::PostChar(c);
                if (enter)
                    PostKey(VK_RETURN, 0x1C, false);
            }
        }
    }

    void HandleArguments(int argc, char** argv)
    {
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "--dev-console") == 0)
                g_argument = true;
        const char* value = getenv("CRASH_DEV_CONSOLE");
        if (value && *value && strcmp(value, "0") != 0)
            g_argument = true;
        if (g_argument)
            Log("[console] enabled by %s", getenv("CRASH_DEV_CONSOLE") ? "CRASH_DEV_CONSOLE" : "--dev-console");
    }

    bool Enabled() { return g_argument || settings::Get().devConsole; }
    bool IsOpen() { return g_open; }

    bool BlocksGameInput()
    {
        return g_open || plat::TickCountMs() - g_closedAt.load() < RELEASE_MS;
    }

    bool OnKeyDown(uintptr_t key, intptr_t lParam)
    {
        bool repeat = (lParam & (1 << 30)) != 0;
        uint32_t scan = uint32_t(lParam >> 16) & 0xFF;
        bool toggleKey = scan == TOGGLE_SCAN && !(lParam & (1 << 24));
        if (!g_open)
        {
            if ((toggleKey || key == VK_F10) && !repeat && Enabled())
            {
                g_swallowChar = toggleKey;
                Open();
                return true;
            }
            return false;
        }
        if (key == VK_F12 || (key >= VK_F1 && key <= VK_F9) || key == VK_F11)
            return false;  // screenshots, overlay... still work
        std::lock_guard lock(g_mutex);
        if (key != VK_TAB)
            g_cycling = false;
        bool ctrl = gpu::window::ControlHeld();
        if (!repeat && (key == VK_ESCAPE || key == VK_F10 || (toggleKey && g_input.empty())))
        {
            g_swallowChar = toggleKey;
            g_mutex.unlock();
            Close();
            g_mutex.lock();
            return true;
        }
        switch (key)
        {
        case VK_RETURN: Submit(); break;
        case VK_BACK:
            if (g_caret > 0)
            {
                size_t from = PrevChar(g_input, g_caret);
                g_input.erase(from, g_caret - from);
                g_caret = from;
            }
            break;
        case VK_DELETE:
            if (g_caret < g_input.size())
                g_input.erase(g_caret, NextChar(g_input, g_caret) - g_caret);
            break;
        case VK_LEFT: g_caret = PrevChar(g_input, g_caret); break;
        case VK_RIGHT: g_caret = NextChar(g_input, g_caret); break;
        case VK_HOME: g_caret = 0; break;
        case VK_END: g_caret = g_input.size(); break;
        case VK_UP: HistoryStep(-1); break;
        case VK_DOWN: HistoryStep(+1); break;
        case VK_TAB: Complete(); break;
        case VK_PRIOR: g_scroll = std::min<int>(g_scroll + 8, std::max<int>(int(g_lines.size()) - 1, 0)); break;
        case VK_NEXT: g_scroll = std::max(g_scroll - 8, 0); break;
        default:
            if (ctrl && key == 'L')
            {
                g_lines.clear();
                g_scroll = 0;
            }
            else if (ctrl && (key == 'C' || key == 'U'))
            {
                g_input.clear();
                g_caret = 0;
            }
            else if (ctrl && key == 'V')
            {
                std::string pasted = gpu::window::ClipboardText();
                std::replace(pasted.begin(), pasted.end(), '\n', ' ');
                pasted.erase(std::remove(pasted.begin(), pasted.end(), '\r'), pasted.end());
                g_input.insert(g_caret, pasted);
                g_caret += pasted.size();
            }
            break;
        }
        g_generation++;
        return true;
    }

    bool OnChar(wchar_t c, intptr_t lParam, bool dead)
    {
        if (!g_open && !g_swallowChar)
            return false;
        uint32_t scan = uint32_t(lParam >> 16) & 0xFF;
        if (g_swallowChar)
        {
            g_swallowChar = false;
            if (scan == TOGGLE_SCAN)
            {
                // On layouts where that key is a dead key (` on US-International) the layout keeps
                // it pending and would put it in front of the next letter: flush it.
                if (dead)
                    gpu::window::FlushDeadKey();
                return true;
            }
        }
        if (!g_open)
            return false;
        if (dead)
            return true;  // the accent waits for the next key (WM_CHAR brings the composed letter)
        if (c < 32 || c == 127)
            return true;  // Enter, Tab, Backspace, Ctrl+letters: handled as keys
        std::lock_guard lock(g_mutex);
        g_cycling = false;
        std::string text = Utf8(std::wstring(1, c));
        g_input.insert(g_caret, text);
        g_caret += text.size();
        g_historyIndex = -1;
        g_generation++;
        return true;
    }

    bool OnWheel(int notches)
    {
        if (!g_open)
            return false;
        std::lock_guard lock(g_mutex);
        g_scroll = std::clamp(g_scroll + notches * 3, 0, std::max<int>(int(g_lines.size()) - 1, 0));
        g_generation++;
        return true;
    }

    void Print(const std::string& text, Tone tone)
    {
        std::lock_guard lock(g_mutex);
        // "erro: " starts the Lua errors of _TRACEBACK (dev_tools.cpp; the log keeps it as is).
        if (tone == Tone::Error && text.rfind("erro: ", 0) == 0)
        {
            g_lastTraceback = text.substr(6);
            AddLine(Tr(T_ERROR) + g_lastTraceback, tone);
        }
        else
            AddLine(text, tone);
        if (tone != Tone::Input)
            Log("[console] %s", text.c_str());
    }

    int32_t OverrideExit(PPCContext& ctx, uint8_t* base, uint32_t tree, uint32_t node, int32_t exit)
    {
        if (exit >= 0 || g_levelRequest < 0 || (node != NODE_HUB && node != NODE_MAIN_MENU))
            return exit;
        int32_t index = g_levelRequest.exchange(-1);
        PPCContext call = ctx;
        call.r3.u64 = tree;
        call.r4.s64 = index;
        sub_820F5D58(call, base);
        Log("[console] level %d: StartLevel + SkipToLevel from node %u", index, node);
        return EXIT_SKIP_TO_LEVEL;
    }

    void RunPending(PPCContext& ctx, uint8_t* base)
    {
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            LoadScript();
        }
        for (size_t i = 0; i < g_script.size(); i++)
        {
            ScriptEntry& entry = g_script[i];
            if (entry.done)
                continue;
            int64_t when = boot::Resolve(entry.at);
            if (when < 0 || input::ClockMs() < when)
                break;  // in order: a later entry waits for this one
            entry.done = true;
            std::string name = "console_" + std::to_string(i + 1);
            boot::Event(name.c_str(), entry.text.c_str());
            RunScriptEntry(entry.text);
        }
        if (g_levels.empty())
        {
            std::vector<std::string> levels = Levels();
            std::lock_guard lock(g_mutex);
            g_levels = levels;
        }
        while (true)
        {
            std::string line;
            {
                std::lock_guard lock(g_mutex);
                if (g_queue.empty())
                    break;
                line = g_queue.front();
                g_queue.pop_front();
            }
            Execute(ctx, base, line);
            boot::Event("console_cmd", line.c_str());
        }
    }

    // ---- drawing ----
    namespace
    {
        struct Colour
        {
            uint8_t r, g, b;
        };

        Colour ToneColour(Tone tone)
        {
            switch (tone)
            {
            case Tone::Input: return { 255, 196, 92 };
            case Tone::Ok: return { 128, 228, 128 };
            case Tone::Error: return { 255, 104, 92 };
            case Tone::Lua: return { 150, 204, 255 };
            case Tone::Help: return { 240, 230, 160 };
            case Tone::Dim: return { 150, 150, 160 };
            default: return { 225, 225, 225 };
            }
        }

        struct Canvas
        {
            plat::TextCanvas text;
            uint32_t* bits = nullptr;
            int w = 0, h = 0, fontHeight = 0, charW = 0, lineH = 0;
            std::vector<uint32_t> pixels;
            std::vector<uint8_t> rowTone;  // Tone per pixel row (+1; 0 = background)
        };
        Canvas g_canvas;
        uint64_t g_drawnVersion = 0;

        bool Resize(int w, int h)
        {
            Canvas& c = g_canvas;
            int fontHeight = std::clamp(h / 26, 13, 36);
            if (c.w == w && c.h == h && c.fontHeight == fontHeight && c.bits)
                return true;
            if (!c.text.SetFont(plat::FontFace::Monospace, fontHeight) || !c.text.Resize(w, h))
                return false;
            int lineHeight;
            c.text.Metrics(c.charW, lineHeight);
            c.lineH = lineHeight + 2;
            c.bits = c.text.Pixels();
            c.w = w;
            c.h = h;
            c.fontHeight = fontHeight;
            c.pixels.assign(size_t(w) * h, 0);
            c.rowTone.assign(size_t(h), 0);
            return true;
        }

        void Text(int x, int y, const std::string& text, Tone tone)
        {
            Canvas& c = g_canvas;
            c.text.Draw(x, y, Wide(text));
            for (int row = std::max(y, 0); row < std::min(y + c.lineH, c.h); row++)
                c.rowTone[size_t(row)] = uint8_t(int(tone) + 1);
        }

        // Splits a line into rows of at most `columns` characters.
        std::vector<std::string> Wrap(const std::string& text, int columns)
        {
            std::vector<std::string> rows;
            std::wstring wide = Wide(text);
            if (columns <= 0)
                columns = 1;
            for (size_t at = 0; at < wide.size() || rows.empty(); at += size_t(columns))
            {
                rows.push_back(Utf8(wide.substr(at, size_t(columns))));
                if (wide.empty())
                    break;
            }
            return rows;
        }

        void Render(const std::vector<Line>& lines, const std::string& input, size_t caret, int scroll, bool caretOn)
        {
            Canvas& c = g_canvas;
            c.text.Clear();
            std::fill(c.rowTone.begin(), c.rowTone.end(), uint8_t(0));
            int pad = c.charW;
            int columns = std::max((c.w - 2 * pad) / c.charW, 8);

            // Header.
            Text(pad, 2, Tr(T_HEADER), Tone::Dim);
            int top = c.lineH + 4;
            // Input line at the bottom.
            int inputY = c.h - c.lineH - 4;
            std::wstring prompt = L"> ";
            std::wstring wideInput = Wide(input);
            size_t caretChars = Wide(input.substr(0, caret)).size();
            int inputColumns = columns - int(prompt.size()) - 1;
            size_t first = caretChars > size_t(inputColumns) ? caretChars - size_t(inputColumns) : 0;
            std::wstring shown = wideInput.substr(first, size_t(inputColumns) + 1);
            Text(pad, inputY, Utf8(prompt + shown), Tone::Input);
            if (caretOn)
            {
                int x = pad + int(prompt.size() + caretChars - first) * c.charW;
                c.text.Flush();
                for (int row = std::max(inputY + 1, 0); row < std::min(inputY + c.lineH - 2, c.h); row++)
                    for (int col = std::max(x, 0); col < std::min(x + std::max(2, c.charW / 4), c.w); col++)
                        c.bits[size_t(row) * c.w + col] = 0x00FFFFFFu;
            }
            // Output, newest at the bottom, `scroll` lines up.
            int y = inputY - c.lineH - 4;
            int skip = scroll;
            for (size_t i = lines.size(); i-- > 0 && y >= top;)
            {
                if (skip > 0)
                {
                    skip--;
                    continue;
                }
                std::vector<std::string> rows = Wrap(lines[i].text, columns);
                for (size_t r = rows.size(); r-- > 0 && y >= top;)
                {
                    Text(pad, y, rows[r], lines[i].tone);
                    y -= c.lineH;
                }
            }
            if (scroll > 0)
            {
                std::string mark = "[+" + std::to_string(scroll) + " " + Tr(T_SCROLLED) + "]";
                Text(c.w - pad - (int(Wide(mark).size()) + 1) * c.charW, inputY, mark, Tone::Dim);
            }
            c.text.Flush();

            // Coverage (white text on black) -> straight-alpha RGBA over the panel's background.
            const Colour background{ 14, 16, 24 };
            const uint32_t backgroundAlpha = 226;
            for (int row = 0; row < c.h; row++)
            {
                Colour tone = c.rowTone[size_t(row)] ? ToneColour(Tone(c.rowTone[size_t(row)] - 1)) : Colour{ 255, 255, 255 };
                bool rule = row == c.h - 2 || row == c.h - 1;                    // accent line at the bottom
                bool separator = row == inputY - 3;                              // above the input line
                const uint32_t* source = c.bits + size_t(row) * c.w;
                uint32_t* out = c.pixels.data() + size_t(row) * c.w;
                for (int x = 0; x < c.w; x++)
                {
                    if (rule)
                    {
                        out[x] = 0xFF0090F0u;  // orange (R F0, G 90)
                        continue;
                    }
                    uint32_t coverage = source[x] & 0xFF;
                    Colour base = separator ? Colour{ 70, 72, 84 } : background;
                    uint32_t r = (base.r * (255 - coverage) + tone.r * coverage) / 255;
                    uint32_t g = (base.g * (255 - coverage) + tone.g * coverage) / 255;
                    uint32_t b = (base.b * (255 - coverage) + tone.b * coverage) / 255;
                    uint32_t a = backgroundAlpha + (255 - backgroundAlpha) * coverage / 255;
                    out[x] = r | (g << 8) | (b << 16) | (a << 24);
                }
            }
        }
    }

    void Draw(uint32_t width, uint32_t height)
    {
        if (!g_open || width < 64 || height < 64)
            return;
        int w = int(width), h = std::max(int(height * 0.46f), 120);
        if (!Resize(w, h))
            return;
        bool caretOn = (plat::TickCountMs() / 530) % 2 == 0;
        std::vector<Line> lines;
        std::string input;
        size_t caret;
        int scroll;
        uint64_t generation;
        {
            std::lock_guard lock(g_mutex);
            size_t from = g_lines.size() > 80 + size_t(g_scroll) ? g_lines.size() - 80 - size_t(g_scroll) : 0;
            lines.assign(g_lines.begin() + ptrdiff_t(from), g_lines.end());
            input = g_input;
            caret = g_caret;
            scroll = g_scroll;
            generation = g_generation;
        }
        uint64_t version = (generation << 20) ^ (uint64_t(w) << 4) ^ (uint64_t(h) << 34) ^ (caretOn ? 1 : 0);
        if (version != g_drawnVersion)
        {
            Render(lines, input, std::min(caret, input.size()), scroll, caretOn);
            g_drawnVersion = version;
        }
        gpu::overlay::DrawBitmap(width, height, g_canvas.pixels.data(), w, h, 0, 0, version, gpu::overlay::BITMAP_CONSOLE);
    }
}
