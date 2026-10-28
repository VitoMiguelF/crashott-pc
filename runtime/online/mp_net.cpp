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
// Behaviours by type (CActor::GetBehaviour sub_82142608, the type is each class's vf05): 2 = the
// fight tree (+28 -> its state: +132 state time, +136 sub-state time, +140 general time),
// 3 = physics, 38 = render.
//
// Wire record (64 bytes, raw floats; docs/internals/multiplayer.md has the compact layout):
//   u64 name hash, f32 matrix rows 0-3 xyz (48), f32 fight state time, u32 flags (bit 0 = player)

#include "mp.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
PPC_EXTERN_FUNC(sub_82142608);             // CActor::GetBehaviour(actor, type)

namespace
{
    constexpr uint32_t MAGIC = 0x434D5031;  // "CMP1"
    constexpr size_t RECORD = 64;
    constexpr size_t HEADER = 16;           // magic, sequence, record count, host frame
    constexpr size_t MAX_PACKET = 1200;     // under the usual internet MTU
    constexpr uint32_t CACTOR_VTABLE = 0x820007A4;
    constexpr uint32_t CHARACTER_FIGHT_TREE_VTABLE = 0x82010474;
    constexpr uint32_t FIGHT_TREE_VTABLE = 0x82011B14;

    // Heap pointers the game hands out: virtual (0x40000000..) or physical (0xA0000000..) memory.
    bool GuestPointer(uint32_t p) { return (p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000); }

    enum class Role { None, Host, Client };

    struct Record
    {
        uint64_t name = 0;
        float m[12]{};     // rows 0-3, xyz
        float stateTime = 0;
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
    };

    Net& N()
    {
        static Net net = [] {
            Net n;
            const char* role = getenv("CRASH_MP_ROLE");
            if (!mp::Enabled() || !role)
                return n;
            n.role = !strcmp(role, "host") ? Role::Host : !strcmp(role, "client") ? Role::Client : Role::None;
            if (n.role == Role::None)
                return n;
            if (const char* hz = getenv("CRASH_MP_HZ"))
                n.hz = std::max(1, atoi(hz));
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
                return n;
            }
            n.peer.sin_family = AF_INET;
            n.peer.sin_port = htons(uint16_t(n.role == Role::Host ? port + 1 : port));
            inet_pton(AF_INET, peer && *peer ? peer : "127.0.0.1", &n.peer.sin_addr);
            Log("[mp] net: %s on UDP %d, peer %s:%d, %d Hz, suppress ai %d objectives %d",
                n.role == Role::Host ? "host" : "client", ntohs(local.sin_port), peer && *peer ? peer : "127.0.0.1",
                ntohs(n.peer.sin_port), n.hz, int(n.suppressAi), int(n.suppressObjectives));
            return n;
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

    uint32_t FightState(PPCContext& ctx, uint8_t* base, uint32_t actor)
    {
        PPCContext call = ctx;
        call.r3.u64 = actor;
        call.r4.u64 = 2;
        sub_82142608(call, base);
        uint32_t fight = call.r3.u32;
        if (!fight || (LoadBE32(fight) != CHARACTER_FIGHT_TREE_VTABLE && LoadBE32(fight) != FIGHT_TREE_VTABLE))
            return 0;
        uint32_t state = LoadBE32(fight + 28);
        return GuestPointer(state) ? state : 0;
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
        uint32_t state = FightState(ctx, base, actor);
        r.stateTime = state ? LoadF(state + 132) : 0;
        r.flags = 0;
        for (uint32_t player : mp::PlayerActors())
            if (player == actor)
                r.flags |= 1;
        return true;
    }

    bool Changed(const Record& a, const Record& b)
    {
        for (int i = 0; i < 12; ++i)
            if (std::abs(a.m[i] - b.m[i]) > 1e-3f)
                return true;
        return a.stateTime != b.stateTime;
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

    void HostTick(PPCContext& ctx, uint8_t* base, Net& n)
    {
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
            memcpy(&bits, &r.stateTime, 4);
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

        if (now - n.lastStats >= 5000)
        {
            double seconds = n.lastStats ? (now - n.lastStats) / 1000.0 : 5.0;
            Log("[mp] host: %zu actors in the stream manager, %.1f snapshots/s, %.1f records/s, %.1f packets/s, "
                "%.1f KB/s (%.1f kbit/s) raw; compact estimate %.1f KB/s",
                n.lastActorCount, n.snapshots / seconds, n.records / seconds, n.packets / seconds, n.bytes / seconds / 1024,
                n.bytes * 8 / seconds / 1000, (n.records * 24.0 + n.packets * 36.0) / seconds / 1024);
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
            if (got < int(HEADER))
                break;
            if (Get32(buffer) != MAGIC)
                continue;
            uint32_t sequence = Get32(buffer + 4), count = Get32(buffer + 8);
            if (n.lastSequence && sequence > n.lastSequence + 1)
                n.lost += sequence - n.lastSequence - 1;
            if (sequence > n.lastSequence)
                n.lastSequence = sequence;
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
                memcpy(&r.stateTime, &bits, 4);
                r.flags = Get32(p + 60);
                n.latest[r.name] = r;
                n.received++;
            }
        }
    }

    // After the client's own actor update: every mirrored actor gets the host's transform.
    void ClientApply(Net& n)
    {
        if (n.latest.empty())
            return;
        bool refreshed = false;
        for (auto& [name, r] : n.latest)
        {
            auto it = n.local.find(name);
            if (it == n.local.end() && !refreshed)
            {
                n.local.clear();
                for (uint32_t actor : Actors())
                    n.local[NameOf(actor)] = actor;
                refreshed = true;
                it = n.local.find(name);
            }
            if (it == n.local.end())
            {
                n.missing++;
                continue;
            }
            uint32_t xform = LoadBE32(it->second + 44);
            if (!xform)
                continue;
            for (int row = 0; row < 4; ++row)
                for (int c = 0; c < 3; ++c)
                    StoreF(xform + row * 16 + c * 4, r.m[row * 3 + c]);
            n.applied++;
        }
        uint64_t now = mp::NowMs();
        if (now - n.lastStats >= 5000)
        {
            const Record* player = nullptr;
            for (auto& [name, r] : n.latest)
                if (r.flags & 1)
                    player = &r;
            Log("[mp] client: %zu actors mirrored (%llu writes, %llu without a local actor), %llu records received, "
                "%u packets lost; host player at (%.1f, %.1f, %.1f)",
                n.latest.size(), (unsigned long long)n.applied, (unsigned long long)n.missing,
                (unsigned long long)n.received, n.lost, player ? player->m[9] : 0.f, player ? player->m[10] : 0.f,
                player ? player->m[11] : 0.f);
            n.lastStats = now;
            n.applied = n.missing = 0;
        }
    }
}

namespace mp
{
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
        ClientApply(N());
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
