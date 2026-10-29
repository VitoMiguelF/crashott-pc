#pragma once

// MULTIPLAYER sessions (docs/internals/multiplayer.md, "Lobby"): what the main menu's MULTIPLAYER
// screens (mp_menu.h) open. A host takes UDP port 47810 (CRASH_MP_PORT); a client any free port. The
// lobby speaks "CML1" packets on the game's own socket (mp.h, net), handled on its I/O thread:
//
//   DISCOVER  client -> broadcast (the LAN) and 127.0.0.1: who hosts here?
//   ANNOUNCE  host -> that client: its name, players, maximum, password or not, in a level or not
//   HELLO     client -> host: a token (random), the player's name, the password and the colour it
//             wants (mp_colors.h), every 0.5 s
//   WELCOME   host -> client: its slot (1-3); DENY: full, wrong password, kicked, in a level, version
//   ROSTER    host -> clients, every 0.5 s: the players (slot, token, name, ping, colour), the options and the
//             level the host started (a counter: a client follows each new one once) and the cutscenes
//             it skipped (a counter and the cutscene's frontend node)
//   PING/PONG client <-> host every 0.5 s (round trip shown in the roster; PING also carries the colour
//             the player wants now); 6 s of silence drops one
//   LEAVE     client -> host
//
// An invite code is the host's address in 7 letters ("XXXX-XXX", Crockford base 32; 10 letters,
// "XXXXX-XXXXX", with a port other than 47810); an address ("192.168.0.10" or "192.168.0.10:47810")
// works where a code does. The host's code is its public address once its router forwards the port
// (UPnP) or STUN found one (mp_nat.h), its LAN address otherwise; there is no relay server.
//
//   CRASH_MP_NAME=<name>      the player's name (default: the OS user's, on Android the device's model;
//                             A-Z 0-9, 12 letters)
//   CRASH_MP_PASSWORD=<text>  tests: the host's password, and what a client's password field starts with
//   CRASH_MP_CODE=<code>      tests: what ENTER CODE starts with (mp_menu.cpp)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::lobby
{
    constexpr uint32_t MAGIC = 0x434D4C31;  // "CML1"
    constexpr int DEFAULT_PORT = 47810;
    constexpr uint32_t MAX_PLAYERS = 4;

    enum class State { Off, Hosting, Searching, Joining, Joined, Failed };
    enum class Reason : uint8_t { None, Full, Password, Kicked, Started, Version, NoAnswer, HostLeft, PortTaken };

    struct Player
    {
        uint32_t slot = 0;
        std::string name;
        uint32_t pingMs = 0;
        bool me = false;
        uint8_t colour = 0xFF;  // mp_colors.h (0xFF: none yet)
    };

    struct Server
    {
        uint32_t ip = 0;
        int port = 0;
        std::string name;
        uint32_t players = 0, maxPlayers = 0;
        bool locked = false, inGame = false;
    };

    // The game options the host sets in the lobby (sent in the roster).
    struct Options
    {
        uint32_t maxPlayers = MAX_PLAYERS;  // 2-4
        uint32_t knockOutSeconds = 5;       // 3, 5 or 10: how long a knocked-out player lies
    };

    // Menu side (any thread).
    bool Host();
    void Search();  // opens a socket and asks the LAN every second
    std::vector<Server> Servers();
    bool Join(const std::string& codeOrAddress, const std::string& password);
    void Leave();   // closes the session (and the socket)
    State GetState();
    Reason LastReason();
    const char* ReasonText(Reason reason, bool portuguese);
    std::vector<Player> Players();
    std::string InviteCode();  // the host's (or the joined host's) code: the internet one when there is one
    std::string LocalCode();   // host: the code of its LAN address
    const char* InternetText(bool portuguese);  // host: whether the internet can reach it (mp_nat.h)
    std::string Address();     // "192.168.0.10:47810"
    std::string LocalName();
    void Kick(uint32_t slot);
    void SetPassword(const std::string& password);
    std::string Password();
    Options GetOptions();
    void SetOptions(const Options& options);
    bool HostInGame();  // the host started a level

    // Game thread (frontend_hooks.cpp / mp_menu.cpp).
    void HostLevelStarted(int32_t levelIndex);
    void HostBackInMenus();
    bool TakeStart(int32_t& levelIndex);  // client: a level the host started, once
    bool TakeHostLeftLevel();            // client: the host went back to its menus, once
    // Cutscenes (settings/skip_cinematics.cpp): only the host skips one; its skip (the frontend node of
    // the cutscene) goes to the clients in the roster, and each skips that cutscene when it plays it.
    void HostSkippedCutscene(uint32_t node);
    bool TakeCutsceneSkip(uint32_t node);  // client: the host skipped this one (once; kept 20 s)
    bool IsClient();                       // joined to a host
    uint32_t KnockOutSeconds();          // 0 = the game's own

    // Codes.
    std::string EncodeCode(uint32_t ip, int port);
    bool DecodeAddress(const std::string& text, uint32_t& ip, int& port);
    std::string FormatAddress(uint32_t ip, int port);

    // The I/O thread (mp_net.cpp).
    void OnPacket(uint32_t ip, int port, const uint8_t* data, size_t size);
    void IoTick();
}
