// Discord Rich Presence (discord_presence.h).
//
// The protocol of the Discord client's local IPC (the one discord-rpc spoke): frames of a little-endian
// header { uint32 opcode, uint32 length } and `length` bytes of JSON. Opcodes: 0 HANDSHAKE
// {"v":1,"client_id":"<application id>"} (answered by a FRAME with "evt":"READY", or a CLOSE with the
// reason, e.g. an unknown application id), 1 FRAME (commands and their answers), 2 CLOSE, 3 PING (answered
// by 4 PONG with the same payload). SET_ACTIVITY {"cmd":"SET_ACTIVITY","args":{"pid":..,"activity":{..}},
// "nonce":".."} sets the presence, "activity":null clears it; the client allows about 5 per 20 s, so one
// goes at most every 5 s, and only when the text changed. The client removes the presence by itself when
// the connection closes (the game quits).
//
// Joining from Discord: in an online session the activity has a party (its id from the host's invite code, its
// size) and, while the host is not in a level and there is room, the join secret = the invite code. Friends get
// "Join" / "Ask to Join" on the profile; the game SUBSCRIBEs to ACTIVITY_JOIN_REQUEST (a friend asked: accepted
// with SEND_ACTIVITY_JOIN_INVITE; the lobby's password and kick still apply) and ACTIVITY_JOIN (the secret, on
// the friend's side: mp::menu::JoinInvite joins it from the main menu). For a friend whose game is closed,
// Discord starts it through the discord-<application id> URL protocol, registered on Windows at startup
// (discord_ipc_win32.cpp); the game then gets ACTIVITY_JOIN once connected.

#include "discord_presence.h"
#include "discord_ipc.h"
#include "../dev/dev_tools.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"
#include "../gpu/overlay.h"
#include "../online/mp_lobby.h"
#include "../online/mp_menu.h"
#include "../platform/platform.h"
#include "../settings/settings.h"
#include "../settings/title_logo.h"
#include "../settings/ui_text.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace discord
{
    namespace
    {
        // The Discord application (discord.com/developers/applications): its name is the game's name on the
        // profile ("Playing Crash of the Titans"), and its Rich Presence art assets hold the large image.
        // >>> Put the application's id here (or set CRASH_DISCORD_APP_ID); empty = no presence. <<<
        constexpr const char* APPLICATION_ID = "1566112788483678289";  // "Crash of the Titans DE"
        constexpr const char* LARGE_IMAGE = "crashrecomp";  // the art asset's name (the title's icon, private/discord)
        constexpr const char* GAME_NAME = "Crash of the Titans";

        constexpr uint32_t OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4;
        constexpr uint32_t MAX_FRAME = 64 * 1024;
        constexpr uint64_t RETRY_MS = 15000;          // looking for a Discord client again
        constexpr uint64_t RETRY_REFUSED_MS = 120000; // after the client refused the handshake
        constexpr uint64_t READY_TIMEOUT_MS = 5000;
        constexpr uint64_t MIN_UPDATE_MS = 5000;      // the client's rate limit: ~5 updates per 20 s

        // Frontend.bfig nodes (settings/direct_boot.cpp).
        constexpr uint32_t NODE_TITLE = 59, NODE_MAIN_MENU = 70, NODE_HUB = 575;
        // The managers object (sub_821FB4F0): the level index at +24, the levels at +156 (count +316), a
        // level's name ("L1_E1") at +32; the first 20 are the story's episodes, in order (index + 1).
        constexpr uint32_t MANAGERS = 0x824A7A24, LEVEL_INDEX = 24, LEVELS = 156, LEVEL_COUNT = 316, LEVEL_NAME = 32;
        constexpr int32_t STORY_LEVELS = 20;
        // Crash* by player index (online/mp_players.cpp); Crash +300 = the co-op state: 0 not joined, 1 in the
        // backpack, 2 on foot (docs/internals/multiplayer.md).
        constexpr uint32_t CRASH_TABLE = 0x8249D778, COOP_STATE = 300;

        enum class Menu : uint8_t { Starting, Main, Hub };
        std::atomic<Menu> g_menu{ Menu::Starting };  // the last of the main menu (or title) and the HUB shown
        int64_t g_startTime = 0;                     // unix seconds: the game's start (the time played)

        uint64_t NowMs() { return plat::TickCountMs(); }

        std::string ApplicationId()
        {
            const char* id = getenv("CRASH_DISCORD_APP_ID");
            return id && *id ? id : APPLICATION_ID;
        }

        std::string LargeImage()
        {
            const char* asset = getenv("CRASH_DISCORD_ASSET");
            return asset && *asset ? asset : LARGE_IMAGE;
        }

        // JSON string contents (UTF-8 passes as is).
        std::string Escape(const std::string& text)
        {
            std::string out;
            for (unsigned char c : text)
            {
                if (c == '"' || c == '\\')
                    out += '\\', out += char(c);
                else if (c < 0x20)
                {
                    char code[8];
                    snprintf(code, sizeof(code), "\\u%04x", c);
                    out += code;
                }
                else
                    out += char(c);
            }
            return out;
        }

        // ---- the scene ----

        // The level's name ("L1_E1") from the game's memory, empty if none.
        std::string LevelName(int32_t index)
        {
            uint32_t count = LoadBE32(MANAGERS + LEVEL_COUNT);
            if (index < 0 || uint32_t(index) >= count || count > 64)
                return {};
            uint32_t level = LoadBE32(MANAGERS + LEVELS + 4 * uint32_t(index));
            uint32_t name = level >= 0x40000000 ? LoadBE32(level + LEVEL_NAME) : 0;
            if (name < 0x40000000)
                return {};
            const char* text = reinterpret_cast<const char*>(g_memory.Translate(name));
            size_t length = strnlen(text, 32);
            return length < 32 ? std::string(text, length) : std::string();
        }

        // "Episode 1: A NEW HOP": the number and the HUB's title of the episode (the frontend bible's
        // Episode_<level>_Name, as the game shows it; mp::menu::Init asks for them), else the level's name.
        std::string LevelText(int32_t index)
        {
            std::string name = LevelName(index);
            if (index < 0 || index >= STORY_LEVELS)
                return name.empty() ? settings::Tr({ "In a level", "Numa fase", "Dans un niveau", "In einem Level", "En un nivel", "In un livello", "In een level" })
                                    : name;
            std::string title = name.empty() ? std::string() : settings::FrontendText("Episode_" + name + "_Name");
            std::string text = std::string(settings::Tr({ "Episode", "Episódio", "Épisode", "Episode", "Episodio", "Episodio", "Aflevering" })) + " " +
                               std::to_string(index + 1);
            if (!title.empty())
                text += ": " + title;
            return text;
        }

        // Local co-op: player 2 joined the level (in the backpack or on foot).
        bool LocalCoop()
        {
            uint32_t p2 = LoadBE32(CRASH_TABLE + 4);
            bool pointer = (p2 >= 0x40000000 && p2 < 0x7F000000) || (p2 >= 0xA0000000 && p2 < 0xC0000000);  // the heaps (mp_net.cpp)
            return pointer && LoadBE32(p2 + COOP_STATE) != 0;
        }

        // The activity's JSON object.
        std::string Activity()
        {
            std::string details;
            bool inLevel = dev::skins::InLevel();
            if (inLevel)
                details = LevelText(int32_t(LoadBE32(MANAGERS + LEVEL_INDEX)));
            else if (g_menu.load() == Menu::Hub)
                details = "HUB";
            else if (g_menu.load() == Menu::Main)
                details = settings::Tr({ "Main menu", "Menu principal", "Menu principal", "Hauptmenü", "Menú principal", "Menu principale", "Hoofdmenu" });
            else
                details = settings::Tr({ "Starting", "Iniciando", "Démarrage", "Startet", "Iniciando", "Avvio", "Opstarten" });

            std::string json = "{\"details\":\"" + Escape(details) + "\"";
            mp::lobby::State state = mp::lobby::GetState();
            if (state == mp::lobby::State::Hosting || state == mp::lobby::State::Joined)
            {
                // "Online multiplayer (3 of 4)": Discord adds the party's size after the state. The party is the
                // session's (the host's code, the same on every machine); the join secret is that code while a
                // friend can still come in (the lobby refuses joins once the host has started a level).
                size_t players = std::max<size_t>(mp::lobby::Players().size(), 1);
                uint32_t most = std::max<uint32_t>(mp::lobby::GetOptions().maxPlayers, uint32_t(players));
                std::string code = mp::lobby::InviteCode();
                json += ",\"state\":\"" +
                        Escape(settings::Tr({ "Online multiplayer", "Multijogador online", "Multijoueur en ligne", "Online-Mehrspieler", "Multijugador en línea",
                                    "Multigiocatore online", "Online multiplayer" })) +
                        "\",\"party\":{\"id\":\"crashrecomp-" + Escape(code.empty() ? "mp" : code) + "\",\"size\":[" + std::to_string(players) + "," +
                        std::to_string(most) + "]}";
                if (!code.empty() && players < most && !mp::lobby::HostInGame())
                    json += ",\"secrets\":{\"join\":\"" + Escape(code) + "\"}";
            }
            else if (inLevel && LocalCoop())
                json += ",\"state\":\"" +
                        Escape(settings::Tr({ "Local co-op", "Co-op local", "Coop local", "Lokaler Koop", "Cooperativo local", "Cooperativa locale",
                                              "Lokale co-op" })) +
                        "\",\"party\":{\"id\":\"crashrecomp-local\",\"size\":[2,2]}";
            else if (inLevel || g_menu.load() == Menu::Hub)
                json += ",\"state\":\"" +
                        Escape(settings::Tr({ "Playing solo", "Jogando solo", "En solo", "Solo", "Jugando solo", "In solitario", "Solo" })) + "\"";
            json += ",\"timestamps\":{\"start\":" + std::to_string(g_startTime) + "}";
            json += ",\"assets\":{\"large_image\":\"" + Escape(LargeImage()) + "\",\"large_text\":\"" + GAME_NAME + "\"}";
            json += ",\"instance\":false}";
            return json;
        }

        // ---- the connection ----

        bool Send(uint32_t op, const std::string& json)
        {
            uint32_t header[2] = { op, uint32_t(json.size()) };  // little endian on every host we build for
            std::string frame(reinterpret_cast<const char*>(header), sizeof(header));
            frame += json;
            return ipc::Write(frame.data(), frame.size());
        }

        // One frame (Available said bytes are there); false = the connection closed.
        bool Receive(uint32_t& op, std::string& json)
        {
            uint32_t header[2];
            if (!ipc::Read(header, sizeof(header)))
                return false;
            if (header[1] > MAX_FRAME)
            {
                ipc::Close();
                return false;
            }
            op = header[0];
            json.assign(header[1], '\0');
            return header[1] == 0 || ipc::Read(json.data(), json.size());
        }

        // A field's string value in an answer (enough for "message" and "evt"; spaces around the ':' allowed).
        std::string Field(const std::string& json, const char* name)
        {
            std::string key = std::string("\"") + name + "\"";
            for (size_t at = json.find(key); at != std::string::npos; at = json.find(key, at + 1))
            {
                size_t p = at + key.size();
                while (p < json.size() && isspace((unsigned char)json[p]))
                    p++;
                if (p >= json.size() || json[p] != ':')
                    continue;
                p++;
                while (p < json.size() && isspace((unsigned char)json[p]))
                    p++;
                if (p >= json.size() || json[p] != '"')
                    return {};  // null or not a string
                size_t end = json.find('"', ++p);
                return json.substr(p, end == std::string::npos ? std::string::npos : end - p);
            }
            return {};
        }

        uint32_t g_nonce = 0;

        std::string Nonce() { return std::to_string(++g_nonce); }

        // A FRAME from the client: an error, or the events SUBSCRIBEd to (joining from Discord).
        void Event(const std::string& json)
        {
            std::string evt = Field(json, "evt");
            if (evt == "ERROR")
                Log("[discord] error from the Discord client: %s", Field(json, "message").c_str());
            else if (evt == "ACTIVITY_JOIN")
            {
                // This player clicked "Join" (or was accepted): the host's code.
                std::string secret = Field(json, "secret");
                if (secret.empty())
                    return;
                Log("[discord] join from Discord");
                mp::menu::JoinInvite(secret);
                if (g_menu.load() != Menu::Main || dev::skins::InLevel())
                    gpu::overlay::Notice(settings::Tr({ "DISCORD: JOINING FROM THE MAIN MENU", "DISCORD: ENTRANDO PELO MENU PRINCIPAL",
                                                   "DISCORD : CONNEXION DEPUIS LE MENU PRINCIPAL", "DISCORD: BEITRITT UBER DAS HAUPTMENU",
                                                   "DISCORD: UNIENDOSE DESDE EL MENU PRINCIPAL", "DISCORD: INGRESSO DAL MENU PRINCIPALE",
                                                   "DISCORD: DEELNEMEN VIA HET HOOFDMENU" }));
            }
            else if (evt == "ACTIVITY_JOIN_REQUEST")
            {
                // A friend asked to join this session: accepted (the lobby still asks its password, the host can kick).
                size_t user = json.find("\"user\"");
                std::string id = user == std::string::npos ? std::string() : Field(json.substr(user), "id");
                if (id.empty())
                    return;
                Log("[discord] a friend asked to join: invited");
                Send(OP_FRAME, "{\"cmd\":\"SEND_ACTIVITY_JOIN_INVITE\",\"args\":{\"user_id\":\"" + Escape(id) + "\"},\"nonce\":\"" + Nonce() + "\"}");
            }
        }

        // The answers that came: PING -> PONG, CLOSE, errors to the log. False = no connection anymore.
        bool Pump(bool& refused)
        {
            while (ipc::IsOpen())
            {
                long available = ipc::Available();
                if (available <= 0)
                    break;
                uint32_t op = 0;
                std::string json;
                if (!Receive(op, json))
                    break;
                if (op == OP_PING)
                    Send(OP_PONG, json);
                else if (op == OP_CLOSE)
                {
                    Log("[discord] the Discord client closed the connection: %s", Field(json, "message").c_str());
                    refused = true;
                    ipc::Close();
                }
                else if (op == OP_FRAME)
                    Event(json);
            }
            return ipc::IsOpen();
        }

        // Open + handshake + READY. `refused`: the client answered with a CLOSE (an unknown id).
        bool Connect(const std::string& id, bool& refused)
        {
            if (!ipc::Open())
                return false;
            if (!Send(OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" + Escape(id) + "\"}"))
                return false;
            uint64_t until = NowMs() + READY_TIMEOUT_MS;
            while (ipc::IsOpen() && NowMs() < until)
            {
                long available = ipc::Available();
                if (available < 0)
                    break;
                if (available == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                uint32_t op = 0;
                std::string json;
                if (!Receive(op, json))
                    break;
                if (op == OP_FRAME && Field(json, "evt") == "READY")
                {
                    Log("[discord] connected to the Discord client");
                    for (const char* evt : { "ACTIVITY_JOIN", "ACTIVITY_JOIN_REQUEST" })
                        Send(OP_FRAME, std::string("{\"cmd\":\"SUBSCRIBE\",\"evt\":\"") + evt + "\",\"args\":{},\"nonce\":\"" + Nonce() + "\"}");
                    return true;
                }
                if (op == OP_CLOSE)
                {
                    Log("[discord] the Discord client refused the application id %s: %s", id.c_str(), Field(json, "message").c_str());
                    refused = true;
                    break;
                }
            }
            static bool s_logged = false;
            if (!refused && !s_logged)
            {
                s_logged = true;
                Log("[discord] the Discord client did not answer the handshake (READY); trying again later");
            }
            ipc::Close();
            return false;
        }

        std::string SetActivity(const std::string& activity)
        {
            return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(plat::CurrentProcessId()) + ",\"activity\":" + activity +
                   "},\"nonce\":\"" + Nonce() + "\"}";
        }

        void Thread(std::string id)
        {
            plat::SetCurrentThreadName("discord presence");
            uint64_t nextTry = 0, lastSend = 0;
            std::string sent;  // the activity the client has
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                uint64_t now = NowMs();
                if (!settings::Get().discord)
                {
                    if (ipc::IsOpen())
                    {
                        Send(OP_FRAME, SetActivity("null"));
                        ipc::Close();
                        Log("[discord] presence off (game.discord = false)");
                    }
                    sent.clear();
                    continue;
                }
                bool refused = false;
                if (!ipc::IsOpen())
                {
                    if (now < nextTry)
                        continue;
                    nextTry = now + RETRY_MS;
                    if (!Connect(id, refused))
                    {
                        if (refused)
                            nextTry = now + RETRY_REFUSED_MS;
                        continue;
                    }
                    sent.clear();
                }
                if (!Pump(refused))
                {
                    Log("[discord] disconnected from the Discord client");
                    nextTry = now + (refused ? RETRY_REFUSED_MS : RETRY_MS);
                    continue;
                }
                std::string activity = Activity();
                if (activity != sent && now - lastSend >= MIN_UPDATE_MS)
                {
                    if (!Send(OP_FRAME, SetActivity(activity)))
                        continue;
                    Log("[discord] presence: %s", activity.c_str());
                    sent = activity;
                    lastSend = now;
                }
            }
        }
    }

    void Init()
    {
#if defined(__ANDROID__)
        Log("[discord] no Rich Presence on Android (the Discord app has no IPC)");
#else
        std::string id = ApplicationId();
        if (id.empty())
        {
            Log("[discord] no Discord application id (discord_presence.cpp APPLICATION_ID or CRASH_DISCORD_APP_ID): no Rich Presence");
            return;
        }
        // Tests: CRASH_DISCORD_JOIN=<code> = an ACTIVITY_JOIN with that secret.
        if (const char* join = getenv("CRASH_DISCORD_JOIN"); join && *join)
            mp::menu::JoinInvite(join);
        // Discord can start the game for a friend's "Join" (not from the timed test runs: run_game.py's builds).
        if (!getenv("CRASH_TIMEOUT"))
            ipc::RegisterLaunch(id);
        g_startTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::thread(Thread, id).detach();
#endif
    }

    void NoteFrontendNode(uint32_t node)
    {
        if (node == NODE_MAIN_MENU || node == NODE_TITLE)
            g_menu = Menu::Main;
        else if (node == NODE_HUB)
            g_menu = Menu::Hub;
    }
}
