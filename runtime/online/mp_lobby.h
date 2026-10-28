#pragma once

// MULTIPLAYER sessions (docs/internals/multiplayer.md, "Lobby"): what the main menu's MULTIPLAYER
// screens (mp_menu.h) open. A host takes UDP port 47810 (CRASH_MP_PORT); a client any free port. The
// lobby speaks "CML1" packets on the game's own socket (mp.h, net), handled on its I/O thread:
//
//   DISCOVER  client -> broadcast (the LAN) and 127.0.0.1: who hosts here?
//   ANNOUNCE  host -> that client: its name, players, maximum, password or not, in a level or not
//   HELLO     client -> host: a token (random), the player's name and the password, every 0.5 s
//   WELCOME   host -> client: its slot (1-3); DENY: full, wrong password, kicked, in a level, version
//   ROSTER    host -> clients, every 0.5 s: the players (slot, token, name, ping), the options and the
//             level the host started (a counter: a client follows each new one once)
//   PING/PONG client <-> host every 0.5 s (round trip shown in the roster); 6 s of silence drops one
//   LEAVE     client -> host
//
// An invite code is the host's LAN address and port in 10 letters (Crockford base 32, "XXXXX-XXXXX");
// an address ("192.168.0.10" or "192.168.0.10:47810") works where a code does. Over the internet the
// host needs the port forwarded (or a VPN such as a virtual LAN): there is no relay server.
//
//   CRASH_MP_NAME=<name>      the player's name (default: the OS user's, A-Z 0-9, 12 letters)
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
    std::string InviteCode();  // the host's (or the joined host's) code
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
    uint32_t KnockOutSeconds();          // 0 = the game's own

    // Codes.
    std::string EncodeCode(uint32_t ip, int port);
    bool DecodeAddress(const std::string& text, uint32_t& ip, int& port);
    std::string FormatAddress(uint32_t ip, int port);

    // The I/O thread (mp_net.cpp).
    void OnPacket(uint32_t ip, int port, const uint8_t* data, size_t size);
    void IoTick();
}
