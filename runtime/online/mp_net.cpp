// Online multiplayer prototype, M1: the host -> client actor mirror (docs/internals/multiplayer.md).
//
// CRASH_MP_ROLE=host sends, CRASH_MP_HZ times a second (default 30), one UDP snapshot of the actors
// that changed since the last one (all of them once a second) to CRASH_MP_PEER:CRASH_MP_PORT
// (default 127.0.0.1:47810; the client's port is CRASH_MP_PORT + 1). CRASH_MP_ROLE=client receives
// them and, right after the game's own actor update (CActorManager::vf10), writes each actor's
// transform; CRASH_MP_SUPPRESS=ai,objectives also skips the client's CAIManager / CObjectiveManager
// updates (their state comes from the host).
//
// Actors (CStreamManager, see mp_stream.cpp): every room's actor list (room +344..+348) and the
// global room (sm +36, same layout). An actor is found on the other side by its 64-bit name hash at
// actor +8 (the name given to newActor; the same on both machines). actor +40 = its room,
// actor +44 -> its transform: a 4x4 matrix, rows 0-2 the axes, row 3 (+48) the position.
// Enemies run their own AI on both machines (same objectives, same names, same inputs for the
// players): their positions follow the records; their hitpoints are the host's (Hitpoints below).
//
// Wire record (64 bytes, raw floats; docs/internals/multiplayer.md has the compact layout):
//   u64 name hash, f32 matrix rows 0-3 xyz (48), f32 hitpoints (-1 = none known), u32 flags (bit 0 =
//   player, bit 1 = a player not on foot: joining or riding, its position is not where it plays)
//
// M2, the client's input drives a Crash on the host: the client sends player 1's pad (the state the
// game read, 12 bytes, "CMI1" packets, on change and at least every 50 ms) and the host gives it to
// the game as controller CRASH_MP_REMOTE_USER (default 1): the game's own player 2 (B joins the
// backpack, B again leaves it). The client plays as that player: the host's player 2 record goes on
// the client's player 1 (its camera follows it) and the host's player 1 on the client's player 2.
// Animation by input mirroring: the host also sends its player 1's pad, so each machine runs both
// Crashes on the same inputs and the game plays the same moves (run, jump, attacks) on both. The
// client's own Crash runs on its own pad at once (local prediction); the records only correct the
// players' positions, through their physics (TeleportPlayer), when they drift more than CRASH_MP_SNAP
// (default 1.0) away. Both machines start the level with player 2 on foot (mp_players.cpp, RemotePad's
// B taps).

#include "mp.h"
#include "../diagnostics.h"
#include "../kernel/xinput_types.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void CloseSocket(socket_t s) { closesocket(s); }
static bool SetNonBlocking(socket_t s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t INVALID_SOCKET = -1;
static void CloseSocket(socket_t s) { close(s); }
static bool SetNonBlocking(socket_t s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
#endif

extern "C" PPC_FUNC(__imp__sub_82092668);  // CActorManager::vf10 (actors' update)
extern "C" PPC_FUNC(__imp__sub_82099058);  // CAIManager::vf10
extern "C" PPC_FUNC(__imp__sub_8220AC38);  // CObjectiveManager::vf10
PPC_EXTERN_FUNC(sub_82110448);             // CDamageableBehaviour::AdjustHitpoints(this, f1 = delta, r5 = damager)
PPC_EXTERN_FUNC(sub_820972D0);             // CActionTeleport::CActionTeleport(msg, const Vector*, flags)
PPC_EXTERN_FUNC(sub_82097348);             // its destructor
PPC_EXTERN_FUNC(sub_82145358);             // CPhysicsBehaviour: the teleport message (type 40) handler
PPC_EXTERN_FUNC(sub_8222C2D8);             // character drawable (+348): sets its base palette

namespace
{
    constexpr uint32_t MAGIC = 0x434D5031;  // "CMP1"
    constexpr uint32_t INPUT_MAGIC = 0x434D4931;  // "CMI1"
    constexpr uint32_t GAME_DATABASE = 0x824A76A4;  // CGameDatabase: players at +16 / +20
    constexpr uint32_t CRASH_TABLE = 0x8249D778;    // Crash* [2], by player index
    constexpr size_t RECORD = 64;
    constexpr size_t HEADER = 16;           // magic, sequence, record count, host frame
    constexpr size_t MAX_PACKET = 1200;     // under the usual internet MTU
    constexpr uint32_t CACTOR_VTABLE = 0x820007A4;
    // CDamageableBehaviour: its actor at +28, -> hitpoints at +64 (dev/dev_cheats.cpp)
    constexpr uint32_t BEHAVIOUR_ACTOR = 28, DAMAGEABLE_HITPOINTS = 64;

    // Heap pointers the game hands out: virtual (0x40000000..) or physical (0xA0000000..) memory.
    bool GuestPointer(uint32_t p) { return (p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000); }

    enum class Role { None, Host, Client };

    struct Record
    {
        uint64_t name = 0;
        float m[12]{};     // rows 0-3, xyz
        float hitpoints = -1;
        uint32_t flags = 0;
    };

    struct Net
    {
        Role role = Role::None;
        socket_t sock = INVALID_SOCKET;
        sockaddr_in peer{};
        int hz = 30;
        bool suppressAi = false, suppressObjectives = false;
        uint32_t sequence = 0;
        uint64_t lastSend = 0, lastFull = 0, lastStats = 0;
        // host: what was last sent per actor; stats
        std::unordered_map<uint64_t, Record> sent;
        uint64_t bytes = 0, packets = 0, records = 0, snapshots = 0, frames = 0;
        size_t lastActorCount = 0;
        // client: newest record per actor, local actor per name
        std::unordered_map<uint64_t, Record> latest;
        std::unordered_map<uint64_t, uint32_t> local;
        uint64_t applied = 0, missing = 0, received = 0;
        uint32_t lastSequence = 0, lost = 0;
        uint64_t lastSyncTrace = 0, lastPacketAt = 0, teleports = 0;
        // M2: the client's pad on the host / sent by the client
        uint32_t remoteUser = 1;
        std::mutex padMutex;
        XINPUT_GAMEPAD remotePad{};
        uint64_t remotePadAt = 0, padPackets = 0, padSequence = 0;
        XINPUT_GAMEPAD lastSentPad{};
        uint64_t lastPadSend = 0, sentPadSequence = 0;
        float snap = 1.0f;  // client: player position error corrected through the physics
        uint32_t swappedLooks[2] = {};  // client: the players' drawables whose looks were swapped
        uint64_t lastSnap[2] = {};      // client: last correction of each player
        uint64_t backpackSince = 0;  // player 2 riding in the backpack since (RemotePad taps B)
        // Each actor's damageable behaviour, learnt from AdjustHitpoints (it sets every character's
        // hitpoints when it comes up); client: the host's hitpoints being applied, damage dropped
        std::unordered_map<uint32_t, uint32_t> damageables;
        bool applyingHitpoints = false;
        uint64_t hitpointsApplied = 0, damageDropped = 0;
    };

    Net& N()
    {
        static Net& net = *[] {
            Net* made = new Net;
            Net& n = *made;
            const char* role = getenv("CRASH_MP_ROLE");
            if (!mp::Enabled() || !role)
                return made;
            n.role = !strcmp(role, "host") ? Role::Host : !strcmp(role, "client") ? Role::Client : Role::None;
            if (n.role == Role::None)
                return made;
            if (const char* hz = getenv("CRASH_MP_HZ"))
                n.hz = std::max(1, atoi(hz));
            if (const char* user = getenv("CRASH_MP_REMOTE_USER"))
                n.remoteUser = uint32_t(atoi(user));
            if (const char* snap = getenv("CRASH_MP_SNAP"))
                n.snap = float(atof(snap));
            if (const char* s = getenv("CRASH_MP_SUPPRESS"))
            {
                n.suppressAi = strstr(s, "ai") != nullptr;
                n.suppressObjectives = strstr(s, "objectives") != nullptr;
            }
            int port = 47810;
            if (const char* p = getenv("CRASH_MP_PORT"))
                port = atoi(p);
            const char* peer = getenv("CRASH_MP_PEER");
#ifdef _WIN32
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
            n.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(uint16_t(n.role == Role::Host ? port : port + 1));
            if (n.sock == INVALID_SOCKET || bind(n.sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0
                || !SetNonBlocking(n.sock))
            {
                Log("[mp] net: no UDP socket on port %d", ntohs(local.sin_port));
                n.role = Role::None;
                return made;
            }
            n.peer.sin_family = AF_INET;
            n.peer.sin_port = htons(uint16_t(n.role == Role::Host ? port + 1 : port));
            inet_pton(AF_INET, peer && *peer ? peer : "127.0.0.1", &n.peer.sin_addr);
            Log("[mp] net: %s on UDP %d, peer %s:%d, %d Hz, suppress ai %d objectives %d",
                n.role == Role::Host ? "host" : "client", ntohs(local.sin_port), peer && *peer ? peer : "127.0.0.1",
                ntohs(n.peer.sin_port), n.hz, int(n.suppressAi), int(n.suppressObjectives));
            return made;
        }();
        return net;
    }

    float LoadF(uint32_t guest)
    {
        uint32_t bits = LoadBE32(guest);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    void StoreF(uint32_t guest, float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        StoreBE32(guest, bits);
    }

    uint64_t NameOf(uint32_t actor) { return (uint64_t(LoadBE32(actor + 8)) << 32) | LoadBE32(actor + 12); }

    // Every actor the stream manager holds: the global room and each room.
    std::vector<uint32_t> Actors()
    {
        std::vector<uint32_t> out;
        uint32_t sm = mp::StreamManager();
        // Only with the level up: a current room and player 1 (the lists fill while loading).
        if (!sm || !LoadBE32(sm + 636) || mp::PlayerActors().empty())
            return out;
        std::vector<uint32_t> rooms{sm + 36};
        for (uint32_t p = LoadBE32(sm + 20); p && p < LoadBE32(sm + 24); p += 4)
            rooms.push_back(LoadBE32(p));
        for (uint32_t room : rooms)
            for (uint32_t p = LoadBE32(room + 344); p && p < LoadBE32(room + 348); p += 4)
                if (uint32_t actor = LoadBE32(p); actor && LoadBE32(actor) == CACTOR_VTABLE)
                    out.push_back(actor);
        return out;
    }

    // The actor's damageable behaviour, if AdjustHitpoints has seen it (and it is still that actor's).
    uint32_t Damageable(Net& n, uint32_t actor)
    {
        auto it = n.damageables.find(actor);
        if (it == n.damageables.end())
            return 0;
        uint32_t d = it->second;
        if (LoadBE32(d + BEHAVIOUR_ACTOR) != actor || !GuestPointer(LoadBE32(d + DAMAGEABLE_HITPOINTS)))
        {
            n.damageables.erase(it);
            return 0;
        }
        return d;
    }

    // A player's own physics keeps its position and puts the transform back every frame: a player is
    // moved through the physics. CPhysicsBehaviour::Teleport (sub_82142338, what the scripts use) sends
    // a CActionTeleport (message type 40) to the whole actor, and the Crash takes the other player
    // along; here the message goes to the physics' own handler only (sub_82145358: the body, the
    // transform, the stream manager), so only this player moves. The physics behaviour is the player's
    // Crash (0x8249D778[index]) +0xDC; its +96 is the actor.
    bool TeleportPlayer(PPCContext& ctx, uint8_t* base, uint32_t actor, float x, float y, float z)
    {
        uint32_t physics = 0;
        for (uint32_t i = 0; i < 2; ++i)
            if (uint32_t crash = LoadBE32(CRASH_TABLE + i * 4); GuestPointer(crash))
                if (uint32_t candidate = LoadBE32(crash + 0xDC); GuestPointer(candidate) && LoadBE32(candidate + 96) == actor)
                    physics = candidate;
        if (!physics)
            return false;
        static uint32_t message = GuestAlloc(128), vector = GuestAlloc(16);
        if (!message || !vector)
            return false;
        StoreF(vector, x);
        StoreF(vector + 4, y);
        StoreF(vector + 8, z);
        StoreF(vector + 12, 1.0f);
        PPCContext call = ctx;
        call.r3.u64 = message;
        call.r4.u64 = vector;
        call.r5.u64 = 0;
        sub_820972D0(call, base);
        call = ctx;
        call.r3.u64 = physics;
        call.r4.u64 = actor;
        call.r5.u64 = message;
        sub_82145358(call, base);
        call = ctx;
        call.r3.u64 = message;
        sub_82097348(call, base);
        return true;
    }

    // Trace: both players' positions.
    void TracePlayers(const char* role, uint64_t now)
    {
        char line[256];
        int k = snprintf(line, sizeof(line), "[mp] anim %s t=%llu", role, (unsigned long long)now);
        for (uint32_t i = 0; i < 2; ++i)
        {
            uint32_t actor = LoadBE32(GAME_DATABASE + 16 + i * 4);
            uint32_t xf = actor ? LoadBE32(actor + 44) : 0;
            if (!GuestPointer(xf))
                continue;
            uint32_t crash = LoadBE32(CRASH_TABLE + i * 4);
            k += snprintf(line + k, sizeof(line) - k, " | P%u (%.1f, %.1f, %.1f) co-op %u", i + 1, LoadF(xf + 48),
                          LoadF(xf + 52), LoadF(xf + 56), GuestPointer(crash) ? LoadBE32(crash + 300) : 9);
        }
        Log("%s", line);
    }

    // Each player keeps its colour on both machines. The game draws player 1 with the Crash template's
    // normal palette and player 2 with its jacked one ("Crash_Player2.bmp"): the character drawable
    // (CCharacterObject: Crash +0xE0 is the CCharacterRenderBehaviour, its +92 the drawable) copies the
    // normal palette of its variant (+4548; palettes at +4516 normal / +4532 jacked) into its base
    // (+348, sub_8222C2D8) at setup, and blends it every frame toward the jacked one by +0x15D0
    // (sub_822212C0; player 2's stays at the jacked end). The client plays the host's player 2 as its
    // player 1, so the two looks swap there: its player 1 gets the jacked palette as base, its player 2
    // blends toward the normal one. Done once per drawable (a new level makes new ones).
    void SwapLooks(PPCContext& ctx, uint8_t* base, Net& n, uint32_t p1, uint32_t p2)
    {
        if (n.role != Role::Client || n.remoteUser != 1 || !p1 || !p2)
            return;
        uint32_t drawables[2];
        for (uint32_t i = 0; i < 2; ++i)
        {
            uint32_t crash = LoadBE32(CRASH_TABLE + i * 4);
            uint32_t render = GuestPointer(crash) ? LoadBE32(crash + 0xE0) : 0;
            drawables[i] = GuestPointer(render) && LoadBE32(render) == 0x82015BC4 ? LoadBE32(render + 92) : 0;
            if (!GuestPointer(drawables[i]) || LoadBE32(crash + 300) != 2)
                return;  // not both on foot with a drawable yet
        }
        if (drawables[0] == n.swappedLooks[0] && drawables[1] == n.swappedLooks[1])
            return;
        for (uint32_t i = 0; i < 2; ++i)
        {
            uint32_t d = drawables[i], variant = LoadBE32(d + 4548) & 3;
            uint32_t normal = LoadBE32(d + 4516 + variant * 4), jacked = LoadBE32(d + 4532 + variant * 4);
            if (!GuestPointer(normal) || !GuestPointer(jacked))
                return;
            if (i == 0)
            {
                PPCContext call = ctx;
                call.r3.u64 = d + 348;
                call.r4.u64 = jacked + 16;
                sub_8222C2D8(call, base);
            }
            else
                StoreBE32(d + 4532 + variant * 4, normal);
        }
        n.swappedLooks[0] = drawables[0];
        n.swappedLooks[1] = drawables[1];
        Log("[mp] client: looks swapped (player 1 = the host's player 2's colour, player 2 = the host's player 1's)");
    }

    // Trace: the actors spawned after the level came up (the objectives' enemies), name, controller
    // (actor +0x1C) and position.
    void TraceEnemies(const char* role, uint64_t now)
    {
        static std::unordered_map<uint32_t, bool> initial;
        std::vector<uint32_t> actors = Actors();
        if (initial.empty())
        {
            for (uint32_t actor : actors)
                initial[actor] = true;
            return;
        }
        char line[512];
        int k = snprintf(line, sizeof(line), "[mp] enemies %s t=%llu", role, (unsigned long long)now);
        int count = 0;
        for (uint32_t actor : actors)
        {
            uint32_t xf = LoadBE32(actor + 44);
            if (initial.count(actor) || !GuestPointer(xf))
                continue;
            if (++count <= 6 && k < int(sizeof(line)) - 60)
            {
                uint32_t damageable = Damageable(N(), actor);
                k += snprintf(line + k, sizeof(line) - k, " | %08X vt %08X (%.1f, %.1f, %.1f) hp %.1f", LoadBE32(actor + 12),
                              LoadBE32(LoadBE32(actor + 0x1C)), LoadF(xf + 48), LoadF(xf + 52), LoadF(xf + 56),
                              damageable ? LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS)) : -1.0f);
            }
        }
        if (count)
            Log("%s (%d)", line, count);
    }

    bool Read(PPCContext& ctx, uint8_t* base, uint32_t actor, Record& r)
    {
        uint32_t xform = LoadBE32(actor + 44);
        if (!GuestPointer(xform))
            return false;
        r.name = NameOf(actor);
        for (int row = 0; row < 4; ++row)
            for (int c = 0; c < 3; ++c)
                r.m[row * 3 + c] = LoadF(xform + row * 16 + c * 4);
        uint32_t damageable = Damageable(N(), actor);
        r.hitpoints = damageable ? LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS)) : -1.0f;
        r.flags = 0;
        for (uint32_t player : mp::PlayerActors())
            if (player == actor)
                r.flags |= 1;
        for (uint32_t i = 0; i < 2; ++i)
            if (uint32_t crash = LoadBE32(CRASH_TABLE + i * 4); GuestPointer(crash) && LoadBE32(crash + 0xDC)
                && LoadBE32(LoadBE32(crash + 0xDC) + 96) == actor && LoadBE32(crash + 300) != 2)
                r.flags |= 2;
        return true;
    }

    bool Changed(const Record& a, const Record& b)
    {
        for (int i = 0; i < 12; ++i)
            if (std::abs(a.m[i] - b.m[i]) > 1e-3f)
                return true;
        return a.hitpoints != b.hitpoints || a.flags != b.flags;
    }

    void Put32(std::vector<uint8_t>& out, uint32_t v)
    {
        for (int i = 3; i >= 0; --i)
            out.push_back(uint8_t(v >> (i * 8)));
    }

    uint32_t Get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

    void Flush(Net& n, std::vector<uint8_t>& packet, uint32_t count)
    {
        if (!count)
            return;
        packet[8] = uint8_t(count >> 24);
        packet[9] = uint8_t(count >> 16);
        packet[10] = uint8_t(count >> 8);
        packet[11] = uint8_t(count);
        sendto(n.sock, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
               reinterpret_cast<const sockaddr*>(&n.peer), sizeof(n.peer));
        n.bytes += packet.size() + 28;  // + IPv4 and UDP headers
        n.packets++;
    }

    // The peer's player pad ("CMI1", sequence, the 12-byte XINPUT_GAMEPAD as its game read it): the
    // client's on the host, the host's player 1 on the client.
    void ReceivePad(Net& n, const uint8_t* buffer, int got)
    {
        if (got < 8 + int(sizeof(XINPUT_GAMEPAD)) || Get32(buffer) != INPUT_MAGIC)
            return;
        uint32_t sequence = Get32(buffer + 4);
        std::lock_guard<std::mutex> lock(n.padMutex);
        if (sequence <= n.padSequence && n.padSequence - sequence < 1000)
            return;  // late or duplicated
        n.padSequence = sequence;
        uint16_t before = n.remotePad.wButtons;
        memcpy(&n.remotePad, buffer + 8, sizeof(XINPUT_GAMEPAD));
        if (mp::Trace() && n.remotePad.wButtons != before)
            Log("[mp] pad in t=%llu buttons %04X", (unsigned long long)mp::NowMs(), n.remotePad.wButtons);
        n.remotePadAt = mp::NowMs();
        n.padPackets++;
    }

    void HostReceive(Net& n)
    {
        uint8_t buffer[256];
        for (;;)
        {
            int got = int(recv(n.sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
            if (got <= 0)
                break;
            ReceivePad(n, buffer, got);
        }
    }

    // Tests: CRASH_MP_TELEPORT=x,y,z puts both of the host's players there (P2 2 units along x), once,
    // 2 s after both are on foot.
    void HostTestTeleport(PPCContext& ctx, uint8_t* base)
    {
        static float target[3];
        static int state = [] {
            const char* at = getenv("CRASH_MP_TELEPORT");
            return at && sscanf(at, "%f,%f,%f", &target[0], &target[1], &target[2]) == 3 ? 1 : 0;
        }();
        static uint64_t since;
        if (state != 1)
            return;
        uint32_t c1 = LoadBE32(CRASH_TABLE), c2 = LoadBE32(CRASH_TABLE + 4);
        if (!GuestPointer(c1) || !GuestPointer(c2) || LoadBE32(c1 + 300) != 2 || LoadBE32(c2 + 300) != 2)
            return;
        if (!since)
            since = mp::NowMs();
        if (mp::NowMs() - since < 2000)
            return;
        state = 2;
        uint32_t p1 = LoadBE32(GAME_DATABASE + 16), p2 = LoadBE32(GAME_DATABASE + 20);
        bool moved = TeleportPlayer(ctx, base, p1, target[0], target[1], target[2])
                     && TeleportPlayer(ctx, base, p2, target[0] + 2, target[1], target[2]);
        Log("[mp] test teleport of both players to (%.1f, %.1f, %.1f): %s", target[0], target[1], target[2],
            moved ? "done" : "no physics");
    }

    void HostTick(PPCContext& ctx, uint8_t* base, Net& n)
    {
        HostReceive(n);
        HostTestTeleport(ctx, base);
        uint64_t now = mp::NowMs();
        n.frames++;
        if (now - n.lastSend < uint64_t(1000 / n.hz))
            return;
        n.lastSend = now;
        bool full = now - n.lastFull >= 1000;
        if (full)
            n.lastFull = now;

        std::vector<uint32_t> actors = Actors();
        n.lastActorCount = actors.size();
        std::vector<uint8_t> packet;
        uint32_t count = 0;
        auto begin = [&] {
            packet.clear();
            Put32(packet, MAGIC);
            Put32(packet, ++n.sequence);
            Put32(packet, 0);
            Put32(packet, uint32_t(n.frames));
            count = 0;
        };
        begin();
        for (uint32_t actor : actors)
        {
            Record r;
            if (!Read(ctx, base, actor, r))
                continue;
            auto it = n.sent.find(r.name);
            if (!full && it != n.sent.end() && !Changed(it->second, r))
                continue;
            n.sent[r.name] = r;
            Put32(packet, uint32_t(r.name >> 32));
            Put32(packet, uint32_t(r.name));
            for (float f : r.m)
            {
                uint32_t bits;
                memcpy(&bits, &f, 4);
                Put32(packet, bits);
            }
            uint32_t bits;
            memcpy(&bits, &r.hitpoints, 4);
            Put32(packet, bits);
            Put32(packet, r.flags);
            n.records++;
            if (++count == (MAX_PACKET - HEADER) / RECORD)
            {
                Flush(n, packet, count);
                begin();
            }
        }
        Flush(n, packet, count);
        n.snapshots++;
        if (mp::Trace() && now - n.lastSyncTrace >= 250)
        {
            n.lastSyncTrace = now;
            TracePlayers("host", now);
            if (n.frames % 2)
                TraceEnemies("host", now);
        }

        if (now - n.lastStats >= 5000)
        {
            double seconds = n.lastStats ? (now - n.lastStats) / 1000.0 : 5.0;
            Log("[mp] host: %zu actors in the stream manager, %.1f snapshots/s, %.1f records/s, %.1f packets/s, "
                "%.1f KB/s (%.1f kbit/s) raw; compact estimate %.1f KB/s; client pad %.1f packets/s, buttons %04X",
                n.lastActorCount, n.snapshots / seconds, n.records / seconds, n.packets / seconds, n.bytes / seconds / 1024,
                n.bytes * 8 / seconds / 1000, (n.records * 24.0 + n.packets * 36.0) / seconds / 1024,
                n.padPackets / seconds, n.remotePad.wButtons);
            n.padPackets = 0;
            n.lastStats = now;
            n.snapshots = n.records = n.packets = n.bytes = 0;
        }
    }

    void ClientReceive(Net& n)
    {
        uint8_t buffer[2048];
        for (;;)
        {
            int got = int(recv(n.sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
            if (got <= 0)
                break;
            if (got < int(HEADER) || Get32(buffer) != MAGIC)
            {
                ReceivePad(n, buffer, got);
                continue;
            }
            uint32_t sequence = Get32(buffer + 4), count = Get32(buffer + 8);
            if (n.lastSequence && sequence > n.lastSequence + 1)
                n.lost += sequence - n.lastSequence - 1;
            if (sequence > n.lastSequence)
                n.lastSequence = sequence;
            n.lastPacketAt = mp::NowMs();
            const uint8_t* p = buffer + HEADER;
            for (uint32_t i = 0; i < count && p + RECORD <= buffer + got; ++i, p += RECORD)
            {
                Record r;
                r.name = (uint64_t(Get32(p)) << 32) | Get32(p + 4);
                for (int k = 0; k < 12; ++k)
                {
                    uint32_t bits = Get32(p + 8 + k * 4);
                    memcpy(&r.m[k], &bits, 4);
                }
                uint32_t bits = Get32(p + 56);
                memcpy(&r.hitpoints, &bits, 4);
                r.flags = Get32(p + 60);
                n.latest[r.name] = r;
                n.received++;
            }
        }
    }

    // After the client's own actor update: every mirrored actor gets the host's transform.
    void ClientApply(PPCContext& ctx, uint8_t* base, Net& n)
    {
        if (n.latest.empty())
            return;
        // The client plays the host's player 2: the two players' records swap actors.
        uint32_t p1 = LoadBE32(GAME_DATABASE + 16), p2 = LoadBE32(GAME_DATABASE + 20);
        uint64_t p1Name = p1 ? NameOf(p1) : 0, p2Name = p2 ? NameOf(p2) : 0;
        // The local actors from the live room lists, every frame: an actor the game removed (an enemy
        // killed, a pickup taken) must not get the host's transform written into its freed memory.
        n.local.clear();
        for (uint32_t actor : Actors())
            n.local[NameOf(actor)] = actor;
        // Players run on their own inputs here (input mirroring); a record only corrects a player that
        // drifted more than `snap` away, through its physics, after the loop.
        struct Target
        {
            uint32_t actor = 0;
            float pos[3]{};
        } targets[2];
        for (auto& [recordName, r] : n.latest)
        {
            uint64_t name = recordName;
            if (p1 && p2 && n.remoteUser == 1)
                name = name == p1Name ? p2Name : name == p2Name ? p1Name : name;
            auto it = n.local.find(name);
            if (it == n.local.end())
            {
                n.missing++;
                continue;
            }
            uint32_t xform = LoadBE32(it->second + 44);
            if (!xform)
                continue;
            // A player not on foot (joining, in the backpack: Crash +300 != 2) is the game's to move.
            if (it->second == p1 || it->second == p2)
                if (uint32_t crash = LoadBE32(CRASH_TABLE + (it->second == p1 ? 0 : 4)); crash && LoadBE32(crash + 300) != 2)
                    continue;
            if (it->second == p1 || it->second == p2)
            {
                float dx = LoadF(xform + 48) - r.m[9], dy = LoadF(xform + 52) - r.m[10], dz = LoadF(xform + 56) - r.m[11];
                uint32_t index = it->second == p1 ? 0 : 1;
                // At most every 250 ms: a player that cannot get there (pushed, hit) is not teleported every frame.
                if (!(r.flags & 2) && dx * dx + dy * dy + dz * dz > n.snap * n.snap && mp::NowMs() - n.lastSnap[index] >= 250)
                {
                    targets[index] = {it->second, {r.m[9], r.m[10], r.m[11]}};
                    n.lastSnap[index] = mp::NowMs();
                }
                continue;
            }
            for (int row = 0; row < 4; ++row)
                for (int c = 0; c < 3; ++c)
                    StoreF(xform + row * 16 + c * 4, r.m[row * 3 + c]);
            n.applied++;
            // The host's hitpoints: its hits (and the deaths they bring) land here too.
            if (uint32_t damageable = r.hitpoints >= 0 ? Damageable(n, it->second) : 0)
            {
                float local = LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS));
                if (local >= 0 && std::abs(local - r.hitpoints) > 0.01f)
                {
                    PPCContext call = ctx;
                    call.r3.u64 = damageable;
                    call.r5.u64 = 0;
                    call.f1.f64 = double(r.hitpoints) - local;
                    n.applyingHitpoints = true;
                    sub_82110448(call, base);
                    n.applyingHitpoints = false;
                    n.hitpointsApplied++;
                    if (mp::Trace())
                        Log("[mp] client: %08X hitpoints %.1f -> %.1f (host)", uint32_t(name), local, r.hitpoints);
                }
            }
        }
        for (const Target& t : targets)
            if (t.actor && TeleportPlayer(ctx, base, t.actor, t.pos[0], t.pos[1], t.pos[2]))
            {
                n.teleports++;
                if (mp::Trace())
                    Log("[mp] snap P%u to (%.1f, %.1f, %.1f)", t.actor == p1 ? 1 : 2, t.pos[0], t.pos[1], t.pos[2]);
            }
        if (mp::Trace() && mp::NowMs() - n.lastSyncTrace >= 250)
        {
            n.lastSyncTrace = mp::NowMs();
            TracePlayers("client", n.lastSyncTrace);
            TraceEnemies("client", n.lastSyncTrace);
        }
        SwapLooks(ctx, base, n, p1, p2);
        uint64_t now = mp::NowMs();
        if (now - n.lastStats >= 5000)
        {
            const Record* player = nullptr;
            for (auto& [name, r] : n.latest)
                if (r.flags & 1)
                    player = &r;
            Log("[mp] client: %zu actors mirrored (%llu writes, %llu without a local actor), %llu records received, "
                "%u packets lost; host player at (%.1f, %.1f, %.1f); %llu player teleports; hitpoints: %llu from the "
                "host, %llu local damages dropped",
                n.latest.size(), (unsigned long long)n.applied, (unsigned long long)n.missing,
                (unsigned long long)n.received, n.lost, player ? player->m[9] : 0.f, player ? player->m[10] : 0.f,
                player ? player->m[11] : 0.f, (unsigned long long)n.teleports, (unsigned long long)n.hitpointsApplied,
                (unsigned long long)n.damageDropped);
            n.teleports = n.hitpointsApplied = n.damageDropped = 0;
            n.lastStats = now;
            n.applied = n.missing = 0;
        }
    }
}

namespace mp
{
    bool Online() { return Enabled() && N().role != Role::None; }

    double Hitpoints(uint32_t damageable, double delta)
    {
        if (!Enabled())
            return delta;
        Net& n = N();
        if (n.role == Role::None)
            return delta;
        uint32_t actor = LoadBE32(damageable + BEHAVIOUR_ACTOR);
        if (!GuestPointer(actor))
            return delta;
        n.damageables[actor] = damageable;
        if (Trace() && delta < 0)
            Log("[mp] %s: %08X takes %.1f (hitpoints %.1f)%s", n.role == Role::Host ? "host" : "client", uint32_t(NameOf(actor)),
                -delta, LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS)), n.applyingHitpoints ? " from the host" : "");
        if (n.role != Role::Client || delta >= 0 || n.applyingHitpoints)
            return delta;
        for (uint32_t player : PlayerActors())
            if (player == actor)
                return delta;  // the players' own hitpoints stay with each machine's game
        // An actor the host mirrors takes its hits from the host's records only.
        if (!n.latest.count(NameOf(actor)))
            return delta;
        n.damageDropped++;
        return 0.0;
    }

    void LocalPad(const void* padBytes, bool poll)
    {
        if (!Enabled())
            return;
        Net& n = N();
        // Only in a level: in the menus the host's title screen would take it as player 1.
        if (n.role == Role::None || !InLevel())
            return;
        // The peer runs this player on the same pad; pause stays local (no START / BACK).
        XINPUT_GAMEPAD pad = *static_cast<const XINPUT_GAMEPAD*>(padBytes);
        pad.wButtons &= ~(0x0010 | 0x0020);
        uint64_t now = NowMs();
        if (!poll || (!memcmp(&pad, &n.lastSentPad, sizeof(pad)) && now - n.lastPadSend < 50))
            return;
        if (Trace() && pad.wButtons != n.lastSentPad.wButtons)
            Log("[mp] pad out t=%llu buttons %04X", (unsigned long long)now, pad.wButtons);
        n.lastSentPad = pad;
        n.lastPadSend = now;
        std::vector<uint8_t> packet;
        Put32(packet, INPUT_MAGIC);
        Put32(packet, uint32_t(++n.sentPadSequence));
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&pad);
        packet.insert(packet.end(), raw, raw + sizeof(pad));
        sendto(n.sock, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
               reinterpret_cast<const sockaddr*>(&n.peer), sizeof(n.peer));
    }

    bool RemotePad(uint32_t user, void* pad)
    {
        if (!Enabled())
            return false;
        Net& n = N();
        // Player 2 joins at the start (mp_players.cpp): its controller stays plugged in, on the peer's
        // pad (the client's on the host, the host's player 1 on the client); a peer that went quiet
        // leaves its Crash standing. While it rides in the backpack (bc +300 = 1) the controller taps B
        // (0.15 s every 1 s) to get it off, on both machines.
        if (n.role == Role::None || user != n.remoteUser || !InLevel())
            return false;
        XINPUT_GAMEPAD& game = *static_cast<XINPUT_GAMEPAD*>(pad);
        game = {};
        {
            std::lock_guard<std::mutex> lock(n.padMutex);
            if (n.remotePadAt && NowMs() - n.remotePadAt <= 2000)
                memcpy(pad, &n.remotePad, sizeof(n.remotePad));
        }
        uint32_t crash = LoadBE32(CRASH_TABLE + 4);
        if (crash && LoadBE32(crash + 300) == 1)
        {
            uint64_t now = NowMs();
            if (!n.backpackSince)
            {
                n.backpackSince = now;
                Log("[mp] player 2 in the backpack: tapping B");
            }
            if ((now - n.backpackSince) % 1000 < 150)
                game.wButtons |= 0x2000;  // B
        }
        else if (n.backpackSince)
        {
            Log("[mp] player 2 off the backpack after %llu ms", (unsigned long long)(NowMs() - n.backpackSince));
            n.backpackSince = 0;
        }
        return true;
    }

    void NetTick(PPCContext& ctx, uint8_t* base)
    {
        Net& n = N();
        if (n.role == Role::Host)
            HostTick(ctx, base, n);
        else if (n.role == Role::Client)
            ClientReceive(n);
    }
}

// CActorManager::vf10: the actors' update; on a client the host's transforms go in right after it.
PPC_FUNC(sub_82092668)
{
    __imp__sub_82092668(ctx, base);
    if (mp::Enabled() && N().role == Role::Client)
        ClientApply(ctx, base, N());
}

// CAIManager::vf10 / CObjectiveManager::vf10: skipped on a client with CRASH_MP_SUPPRESS, once its
// level is up (player 1 spawned by the level's own objectives, the host already sending).
static bool ClientSuppresses(bool what)
{
    return what && mp::Enabled() && N().role == Role::Client && !N().latest.empty() && !mp::PlayerActors().empty();
}

PPC_FUNC(sub_82099058)
{
    if (ClientSuppresses(N().suppressAi))
        return;
    __imp__sub_82099058(ctx, base);
}

PPC_FUNC(sub_8220AC38)
{
    if (ClientSuppresses(N().suppressObjectives))
        return;
    __imp__sub_8220AC38(ctx, base);
}
