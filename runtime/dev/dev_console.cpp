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
#include "../platform/platform.h"

#include <windows.h>

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
        constexpr ULONGLONG RELEASE_MS = 250;  // the game still sees a neutral pad after closing
        constexpr UINT TOGGLE_SCAN = 0x29;     // the key left of 1 (` on US, ' on ABNT2)

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
        std::atomic<ULONGLONG> g_closedAt{ 0 };
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
            int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
            std::wstring wide(size_t(std::max(length, 0)), L'\0');
            if (length > 0)
                MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), wide.data(), length);
            return wide;
        }

        std::string Utf8(const std::wstring& wide)
        {
            int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
            std::string text(size_t(std::max(length, 0)), '\0');
            if (length > 0)
                WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), text.data(), length, nullptr, nullptr);
            return text;
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

        // ---- commands ----
        struct Command
        {
            const char* name;
            const char* usage;
            const char* help;
        };
        const Command COMMANDS[] = {
            { "help", "help [comando]", "lista os comandos, ou explica um" },
            { "unlock", "unlock all | unlock skin <nome>", "desbloqueia tudo (20 skins + Crash_Hulk, fases, golpes) ou uma skin" },
            { "mojo", "mojo <n>", "da n de mojo (o HUD acompanha)" },
            { "god", "god [on|off]", "invencibilidade (o HP nao desce; poços ainda matam)" },
            { "tp", "tp <x> <y> <z>", "teleporta o jogador 1 (y = altura; veja pos)" },
            { "pos", "pos", "mostra a posicao do jogador 1" },
            { "shadow", "shadow [on|off]", "cheat Shadow Crash do jogo" },
            { "bighead", "bighead [on|off]", "cheat Big Head do jogo" },
            { "skin", "skin [<nome> [now]]", "veste uma skin (na fase: na proxima carga; now reinicia a fase); sem nome lista" },
            { "level", "level [<nome>]", "vai para a fase (de dentro de uma fase, do HUB ou do menu principal); sem nome lista" },
            { "reload", "reload", "reinicia a fase atual (UberManager:ForceReload)" },
            { "lua", "lua <codigo> | lua =<expressao>", "roda Lua no estado do jogo (devprint escreve aqui)" },
            { "clear", "clear", "limpa a saida (Ctrl+L)" },
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
                return index == 1 ? std::vector<std::string>{ "all", "skin" }
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
                    AddLine("CrashRecomp - console de desenvolvedor. \"help\" lista os comandos.", Tone::Help);
                    AddLine("Tab completa, setas para cima/baixo = historico, PageUp/PageDown rolam, Esc fecha.", Tone::Dim);
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
                    AddLine("erro: o estado Lua do jogo ainda nao existe", Tone::Error);
                else if (error != g_lastTraceback)
                    AddLine("erro: " + error, Tone::Error);
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
                        Reply(c.usage, Tone::Help);
                        Reply("  " + std::string(c.help), Tone::Normal);
                        if (std::string(c.name) == "skin" || std::string(c.name) == "unlock")
                            Reply("  skins: " + skins::List() + "  ((x) = nao da para usar)", Tone::Dim);
                        if (std::string(c.name) == "level" && !g_levels.empty())
                        {
                            std::string list;
                            for (const std::string& l : g_levels)
                                list += (list.empty() ? "" : " ") + l;
                            Reply("  fases: " + list, Tone::Dim);
                        }
                        return;
                    }
                }
                Reply("comando desconhecido: " + topic, Tone::Error);
                return;
            }
            Reply("Comandos:", Tone::Help);
            for (const Command& c : COMMANDS)
            {
                std::string usage = c.usage;
                usage.resize(std::max<size_t>(usage.size(), 34), ' ');
                Reply("  " + usage + c.help, Tone::Normal);
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
                        Reply(std::string("uso: ") + c.usage, Tone::Error);
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
                    Reply(std::string("skin atual: ") + (current >= 0 ? skins::Label(current) : "?") +
                              (pending >= 0 ? std::string(" (proxima fase: ") + skins::Label(pending) + ")" : ""),
                          Tone::Help);
                    Reply("skins: " + skins::List() + "  ((x) = nao da para usar)", Tone::Dim);
                    return;
                }
                int skin = skins::ByName(words[1]);
                bool now = words.size() > 2 && Lower(words[2]) == "now";
                if (skin < 0)
                    Reply("skin desconhecida: " + words[1] + " (skins: " + skins::List() + ")", Tone::Error);
                else if (!skins::Wearable(skin))
                    Reply(std::string("recusado: ") + skins::Label(skin), Tone::Error);
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
                    Reply(list.empty() ? "a lista de fases ainda nao existe" : "fases: " + list, Tone::Help);
                    return;
                }
                std::string name;
                for (const std::string& l : g_levels)
                    if (_stricmp(l.c_str(), words[1].c_str()) == 0)
                        name = l;
                if (name.empty())
                {
                    Reply("fase desconhecida: " + words[1] + " (\"level\" lista)", Tone::Error);
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
                        Reply("indo para " + name, Tone::Ok);
                    return;
                }
                // In the frontend the level starts at the next evaluation of the HUB or the main
                // menu (OverrideExit): Frontend:StartLevel alone doesn't leave the menu.
                g_levelRequest = int32_t(std::find(g_levels.begin(), g_levels.end(), name) - g_levels.begin());
                Log("[console] level %s: requested from the frontend", name.c_str());
                Reply("indo para " + name + " (pelo HUB ou pelo menu principal)", Tone::Ok);
            }
            else if (command == "reload")
            {
                if (ConsoleLua(ctx, base, "Managers.UberManager:ForceReload()"))
                    Reply("reiniciando a fase", Tone::Ok);
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
                Reply("comando desconhecido: " + words[0] + " (\"help\" lista)", Tone::Error);
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

        // Posts a key to the game window, as a keyboard would (TranslateMessage in the window's
        // loop makes its WM_CHAR from the keyboard layout, as for a real key).
        void PostKey(HWND window, UINT key, UINT scan, bool extended)
        {
            LPARAM down = 1 | (LPARAM(scan) << 16) | (extended ? (1 << 24) : 0);
            PostMessageW(window, WM_KEYDOWN, key, down);
            PostMessageW(window, WM_KEYUP, key, down | (1 << 30) | (LPARAM(1) << 31));
        }

        void RunScriptEntry(const std::string& text)
        {
            HWND window = HWND(gpu::window::Handle());
            if (!window)
                return;
            if (text == "!toggle")
                PostKey(window, MapVirtualKeyW(TOGGLE_SCAN, MAPVK_VSC_TO_VK), TOGGLE_SCAN, false);
            else if (text == "!esc")
                PostKey(window, VK_ESCAPE, 0x01, false);
            else if (text == "!tab")
                PostKey(window, VK_TAB, 0x0F, false);
            else if (text == "!up")
                PostKey(window, VK_UP, 0x48, true);
            else if (text == "!down")
                PostKey(window, VK_DOWN, 0x50, true);
            else if (text == "!enter")
                PostKey(window, VK_RETURN, 0x1C, false);
            else
            {
                bool enter = text.rfind("!type:", 0) != 0;
                for (wchar_t c : Wide(enter ? text : text.substr(6)))
                    PostMessageW(window, WM_CHAR, c, 1);
                if (enter)
                    PostKey(window, VK_RETURN, 0x1C, false);
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

    bool OnKeyDown(WPARAM key, LPARAM lParam)
    {
        bool repeat = (lParam & (1 << 30)) != 0;
        UINT scan = UINT(lParam >> 16) & 0xFF;
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
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
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
            else if (ctrl && key == 'V' && OpenClipboard(nullptr))
            {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
                {
                    if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data)))
                    {
                        std::string pasted = Utf8(text);
                        GlobalUnlock(data);
                        std::replace(pasted.begin(), pasted.end(), '\n', ' ');
                        pasted.erase(std::remove(pasted.begin(), pasted.end(), '\r'), pasted.end());
                        g_input.insert(g_caret, pasted);
                        g_caret += pasted.size();
                    }
                }
                CloseClipboard();
            }
            break;
        }
        g_generation++;
        return true;
    }

    bool OnChar(wchar_t c, LPARAM lParam, bool dead)
    {
        if (!g_open && !g_swallowChar)
            return false;
        UINT scan = UINT(lParam >> 16) & 0xFF;
        if (g_swallowChar)
        {
            g_swallowChar = false;
            if (scan == TOGGLE_SCAN)
            {
                // On layouts where that key is a dead key (` on US-International) the layout keeps
                // it pending and would put it in front of the next letter: flush it.
                if (dead)
                {
                    BYTE keys[256] = {};
                    wchar_t flushed[4];
                    ToUnicode(VK_SPACE, MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC), keys, flushed, 4, 0);
                }
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
        if (tone == Tone::Error && text.rfind("erro: ", 0) == 0)
            g_lastTraceback = text.substr(6);
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
            HDC dc = nullptr;
            HBITMAP bitmap = nullptr;
            HFONT font = nullptr;
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
            int fontHeight = std::clamp(int(GetSystemMetrics(SM_CYSCREEN) > 0 ? h / 26 : 18), 13, 36);
            if (c.w == w && c.h == h && c.fontHeight == fontHeight && c.dc)
                return true;
            if (c.bitmap)
                DeleteObject(c.bitmap);
            if (c.font)
                DeleteObject(c.font);
            if (!c.dc)
                c.dc = CreateCompatibleDC(nullptr);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = w;
            info.bmiHeader.biHeight = -h;  // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            c.bitmap = CreateDIBSection(c.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            c.font = CreateFontW(-fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            if (!c.bitmap || !c.font || !bits)
                return false;
            SelectObject(c.dc, c.bitmap);
            SelectObject(c.dc, c.font);
            SetBkMode(c.dc, TRANSPARENT);
            SetTextColor(c.dc, RGB(255, 255, 255));
            TEXTMETRICW metrics{};
            GetTextMetricsW(c.dc, &metrics);
            c.charW = std::max<int>(metrics.tmAveCharWidth, 1);
            c.lineH = metrics.tmHeight + 2;
            c.bits = static_cast<uint32_t*>(bits);
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
            std::wstring wide = Wide(text);
            TextOutW(c.dc, x, y, wide.c_str(), int(wide.size()));
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
            memset(c.bits, 0, size_t(c.w) * c.h * 4);
            std::fill(c.rowTone.begin(), c.rowTone.end(), uint8_t(0));
            int pad = c.charW;
            int columns = std::max((c.w - 2 * pad) / c.charW, 8);

            // Header.
            Text(pad, 2, "CONSOLE DE DESENVOLVEDOR  -  help: comandos   Tab: completar   ↑↓: historico   Esc/F10: fechar", Tone::Dim);
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
                RECT bar{ x, inputY + 1, x + std::max(2, c.charW / 4), inputY + c.lineH - 2 };
                FillRect(c.dc, &bar, HBRUSH(GetStockObject(WHITE_BRUSH)));
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
                Text(c.w - pad - 12 * c.charW, inputY, "[+" + std::to_string(scroll) + " linhas]", Tone::Dim);
            GdiFlush();

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
