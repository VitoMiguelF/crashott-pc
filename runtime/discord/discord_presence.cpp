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

#include "discord_presence.h"
#include "discord_ipc.h"
#include "../dev/dev_tools.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"
#include "../online/mp_lobby.h"
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
        constexpr const char* APPLICATION_ID = "";
        constexpr const char* LARGE_IMAGE = "crash_icon";  // the art asset's name (the title's icon, private/discord)
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

        // The activity's JSON object.
        std::string Activity()
        {
            std::string details;
            if (dev::skins::InLevel())
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
                // "Online multiplayer (3 of 4)": Discord adds the party's size after the state.
                size_t players = mp::lobby::Players().size();
                uint32_t most = std::max<uint32_t>(mp::lobby::GetOptions().maxPlayers, uint32_t(players));
                json += ",\"state\":\"" +
                        Escape(settings::Tr({ "Online multiplayer", "Multijogador online", "Multijoueur en ligne", "Online-Mehrspieler", "Multijugador en línea",
                                    "Multigiocatore online", "Online multiplayer" })) +
                        "\",\"party\":{\"id\":\"crashrecomp-mp\",\"size\":[" + std::to_string(std::max<size_t>(players, 1)) + "," +
                        std::to_string(most) + "]}";
            }
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
                else if (op == OP_FRAME && Field(json, "evt") == "ERROR")
                    Log("[discord] error from the Discord client: %s", Field(json, "message").c_str());
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

        std::string SetActivity(const std::string& activity, uint32_t nonce)
        {
            return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(plat::CurrentProcessId()) + ",\"activity\":" + activity +
                   "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
        }

        void Thread(std::string id)
        {
            plat::SetCurrentThreadName("discord presence");
            uint64_t nextTry = 0, lastSend = 0;
            uint32_t nonce = 0;
            std::string sent;  // the activity the client has
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                uint64_t now = NowMs();
                if (!settings::Get().discord)
                {
                    if (ipc::IsOpen())
                    {
                        Send(OP_FRAME, SetActivity("null", ++nonce));
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
                    if (!Send(OP_FRAME, SetActivity(activity, ++nonce)))
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
