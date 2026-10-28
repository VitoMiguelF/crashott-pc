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
// Players (2-4, CRASH_MP_PLAYERS): the host is slot 0, each client its CRASH_MP_SLOT (1-3, UDP port +
// slot). Every machine plays its own slot as player 1 (camera, HUD, controller 0) and the other slots
// on players 2-4 in slot order, all native Crashes on foot from the start (mp_players.cpp). Each
// machine sends player 1's pad (the state the game read, "CMI2" packets: slot, sequence, the 12-byte
// XINPUT_GAMEPAD and its camera's forward, on change and at least every 50 ms); the host passes the
// clients' on to the other clients, and each machine gives every remote slot's pad to the controller
// of the player that slot is there. So every machine runs every Crash on the same inputs and the game
// plays the same moves (input mirroring); the local Crash answers at once (local prediction). The
// game walks a player relative to the camera, so a remote left stick is turned from the sender's
// camera to this one's. The host's records only correct the players' positions, through their
// physics (TeleportPlayer), when they drift more than CRASH_MP_SNAP (default 1.0) away.

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
extern "C" PPC_FUNC(__imp__sub_822212C0);  // character drawable: the palette of this frame (+0x11C8)
PPC_EXTERN_FUNC(sub_82110448);             // CDamageableBehaviour::AdjustHitpoints(this, f1 = delta, r5 = damager)
PPC_EXTERN_FUNC(sub_820972D0);             // CActionTeleport::CActionTeleport(msg, const Vector*, flags)
PPC_EXTERN_FUNC(sub_82097348);             // its destructor
PPC_EXTERN_FUNC(sub_82145358);             // CPhysicsBehaviour: the teleport message (type 40) handler
PPC_EXTERN_FUNC(sub_8222C2D8);             // character drawable (+348): sets its base palette

namespace
{
    constexpr uint32_t MAGIC = 0x434D5031;  // "CMP1"
    constexpr uint32_t INPUT_MAGIC = 0x434D4932;  // "CMI2"
    constexpr uint32_t GAME_DATABASE = 0x824A76A4;  // CGameDatabase: players at +16 / +20
    constexpr uint32_t CRASH_TABLE = 0x8249D778;    // Crash* [2], by player index
    constexpr size_t RECORD = 64;
    constexpr size_t HEADER = 16;           // magic, sequence, record count, host frame
    constexpr size_t MAX_PACKET = 1200;     // under the usual internet MTU
    constexpr uint32_t CACTOR_VTABLE = 0x820007A4;
    // CDamageableBehaviour: its actor at +28, -> hitpoints at +64 (dev/dev_cheats.cpp)
    constexpr uint32_t BEHAVIOUR_ACTOR = 28, DAMAGEABLE_HITPOINTS = 64;
    constexpr uint32_t DAMAGEABLE_VTABLE = 0x8201145C;  // CDamageableBehaviour

    // Heap pointers the game hands out: virtual (0x40000000..) or physical (0xA0000000..) memory.
    bool GuestPointer(uint32_t p) { return (p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000); }

    enum class Role { None, Host, Client };

    // Players by slot (the host 0, clients 1-3) and by local index (the game's player index on this
    // machine): each machine plays its own slot as player 1 (camera, HUD, controller 0); the other
    // slots follow in order on players 2-4. Every machine spawns the players in the same order with
    // the same names, so a host record of player index g is slot g.
    uint32_t LocalIndex(uint32_t mine, uint32_t slot) { return slot == mine ? 0 : slot < mine ? slot + 1 : slot; }
    uint32_t SlotOf(uint32_t mine, uint32_t index) { return index == 0 ? mine : index - 1 < mine ? index - 1 : index; }

    uint32_t PlayerActor(uint32_t index)
    {
        uint32_t crash = mp::PlayerCrash(index);
        uint32_t physics = crash ? LoadBE32(crash + 0xDC) : 0;
        uint32_t actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
        return GuestPointer(actor) ? actor : 0;
    }

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
        sockaddr_in peer{};       // client: the host
        sockaddr_in clients[4]{};  // host: each client's address (slot 1-3), learnt from its pads
        uint32_t slot = 0, players = 2;
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
        std::mutex padMutex;  // the pads and the clients' addresses (the input thread reads them)
        XINPUT_GAMEPAD remotePad[4]{};  // by slot
        float remoteForward[4][2]{};    // by slot: the sender's camera forward (x, z) its stick was read with
        uint64_t remotePadAt[4]{}, padSequence[4]{}, padPackets = 0;
        XINPUT_GAMEPAD lastSentPad{};
        uint64_t lastPadSend = 0, sentPadSequence = 0;
        float snap = 1.0f;  // client: player position error corrected through the physics
        uint32_t lookDone[4] = {};  // the players' drawables whose colour was set (by local index)
        uint64_t lastSnap[4] = {};  // client: last correction of each player (by local index)
        float lastRecordPos[4][3] = {}, lastLocalPos[4][3] = {};  // client: to tell a standing player
        uint64_t recordMovedAt[4] = {}, localMovedAt[4] = {};
        // Each actor's damageable behaviour, learnt from AdjustHitpoints (it sets every character's
        // hitpoints when it comes up); client: the host's hitpoints being applied, damage dropped
        std::unordered_map<uint32_t, uint32_t> damageables;
        bool applyingHitpoints = false;
        float recordHitpoints[4] = {-1, -1, -1, -1};
        float lastSnapPos[4][3]{};  // client: the last correction's target, and how many went there in a row
        uint32_t snapRepeats[4]{};  // trace: the host's last hitpoints (fraction) for each player
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
            n.players = mp::Players();
            if (n.role == Role::Client)
            {
                const char* slot = getenv("CRASH_MP_SLOT");
                n.slot = uint32_t(std::clamp(slot ? atoi(slot) : 1, 1, int(n.players) - 1));
            }
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
            local.sin_port = htons(uint16_t(port + n.slot));
            if (n.sock == INVALID_SOCKET || bind(n.sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0
                || !SetNonBlocking(n.sock))
            {
                Log("[mp] net: no UDP socket on port %d", ntohs(local.sin_port));
                n.role = Role::None;
                return made;
            }
            n.peer.sin_family = AF_INET;
            n.peer.sin_port = htons(uint16_t(port));
            inet_pton(AF_INET, peer && *peer ? peer : "127.0.0.1", &n.peer.sin_addr);
            // Until a client's first pad arrives the host sends to CRASH_MP_PEER (127.0.0.1) port + slot.
            for (uint32_t k = 1; k < n.players; ++k)
            {
                n.clients[k] = n.peer;
                n.clients[k].sin_port = htons(uint16_t(port + k));
            }
            Log("[mp] net: %s (player %u of %u) on UDP %d, peer %s, %d Hz, suppress ai %d objectives %d",
                n.role == Role::Host ? "host" : "client", n.slot + 1, n.players, ntohs(local.sin_port),
                peer && *peer ? peer : "127.0.0.1", n.hz, int(n.suppressAi), int(n.suppressObjectives));
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

    // The view camera's forward on the ground (x, z): CRenderManager (manager 12) +356 = the
    // pure3d::VectorCamera, its direction at +228. The game walks a player along stick.x * right +
    // stick.y * forward with right = (forward.z, -forward.x) (input::StickToWorld sub_821EBC70,
    // sub_82253818): a remote player's stick only means the same direction under the same camera.
    bool CameraForward(float& x, float& z)
    {
        uint32_t render = LoadBE32(0x824A7A24 + 28 + 4 * 12);
        uint32_t camera = GuestPointer(render) || (render >= 0x82000000 && render < 0x83000000) ? LoadBE32(render + 356) : 0;
        if (!GuestPointer(camera) && !(camera >= 0x82000000 && camera < 0x83000000))
            return false;
        x = LoadF(camera + 228);
        z = LoadF(camera + 236);
        float length = std::sqrt(x * x + z * z);
        if (!(length > 1e-3f) || !std::isfinite(length))
            return false;
        x /= length;
        z /= length;
        return true;
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

    // A player's damageable before it has taken a hit: one of the pointers of the actor's behaviour controller
    // (actor +0x1C, what Lua calls m_BehaviourController.m_DamageableBehaviour), known by its vtable and
    // its actor (+28).
    uint32_t PlayerDamageable(Net& n, uint32_t actor)
    {
        if (uint32_t d = Damageable(n, actor))
            return d;
        uint32_t controller = GuestPointer(actor) ? LoadBE32(actor + 0x1C) : 0;
        if (!GuestPointer(controller))
            return 0;
        for (uint32_t o = 0; o < 0x400; o += 4)
            if (uint32_t d = LoadBE32(controller + o); GuestPointer(d) && LoadBE32(d) == DAMAGEABLE_VTABLE &&
                                                      LoadBE32(d + BEHAVIOUR_ACTOR) == actor &&
                                                      GuestPointer(LoadBE32(d + DAMAGEABLE_HITPOINTS)))
            {
                n.damageables[actor] = d;
                return d;
            }
        return 0;
    }

    // The host's hitpoints for an actor it mirrors: its hits (and the deaths they bring) land here too.
    void ApplyHitpoints(PPCContext& ctx, uint8_t* base, Net& n, uint32_t actor, const Record& r, uint64_t name, bool player = false)
    {
        uint32_t damageable = r.hitpoints < 0 ? 0 : player ? PlayerDamageable(n, actor) : Damageable(n, actor);
        if (!damageable)
            return;
        float local = LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS)), wanted = r.hitpoints;
        if (player)
        {
            uint32_t max = LoadBE32(damageable + 68);
            if (!GuestPointer(max) || LoadF(max) <= 0)
                return;
            wanted = std::min(r.hitpoints, 1.0f) * LoadF(max);
        }
        if (local < 0 || std::abs(local - wanted) <= 0.01f)
            return;
        PPCContext call = ctx;
        call.r3.u64 = damageable;
        call.r5.u64 = 0;
        call.f1.f64 = double(wanted) - local;
        n.applyingHitpoints = true;
        sub_82110448(call, base);
        n.applyingHitpoints = false;
        n.hitpointsApplied++;
        if (mp::Trace())
            Log("[mp] client: %08X hitpoints %.1f -> %.1f (host)", uint32_t(name), local, wanted);
    }

    // A player's own physics keeps its position and puts the transform back every frame: a player is
    // moved through the physics. CPhysicsBehaviour::Teleport (sub_82142338, what the scripts use) sends
    // a CActionTeleport (message type 40) to the whole actor, and the Crash takes the other player
    // along; here the message goes to the physics' own handler only (sub_82145358: the body, the
    // transform, the stream manager), so only this player moves. The physics behaviour is the player's
    // Crash (0x8249D778[index]) +0xDC; its +96 is the actor.
    bool TeleportPlayer(PPCContext& ctx, uint8_t* base, uint32_t actor, float x, float y, float z)
    {
        std::vector<uint32_t> crashes = mp::ExtraCrashes();
        crashes.push_back(LoadBE32(CRASH_TABLE));
        crashes.push_back(LoadBE32(CRASH_TABLE + 4));
        for (uint32_t crash : crashes)
            if (uint32_t physics = GuestPointer(crash) ? LoadBE32(crash + 0xDC) : 0; GuestPointer(physics) && LoadBE32(physics + 96) == actor)
                return mp::TeleportCrash(ctx, base, crash, x, y, z);
        return false;
    }

    // Trace: both players' positions.
    void TracePlayers(const char* role, uint64_t now)
    {
        char line[400];
        int k = snprintf(line, sizeof(line), "[mp] anim %s t=%llu", role, (unsigned long long)now);
        for (uint32_t i = 0; i < 4; ++i)
        {
            uint32_t actor = PlayerActor(i);
            uint32_t xf = actor ? LoadBE32(actor + 44) : 0;
            if (!GuestPointer(xf))
                continue;
            uint32_t crash = mp::PlayerCrash(i);
            k += snprintf(line + k, sizeof(line) - k, " | P%u (%.1f, %.1f, %.1f) co-op %u hp %.0f (host %.2f)", i + 1, LoadF(xf + 48),
                          LoadF(xf + 52), LoadF(xf + 56), GuestPointer(crash) ? LoadBE32(crash + 300) : 9,
                          mp::PlayerHitpoints(i), N().recordHitpoints[i]);
        }
        Log("%s", line);
    }

    // Drawables whose fur hue is turned (degrees; < 0 none), by FixLooks.
    std::unordered_map<uint32_t, float>& Tints()
    {
        static std::unordered_map<uint32_t, float> tints;
        return tints;
    }
    std::mutex& TintMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    // Crash's fur: the saturated oranges of the normal palette (hue 5-45 degrees, saturation > 0.5; the
    // skin, the jeans and the shoes are outside it) go to `hue`, saturation and value kept.
    void TintPalette(uint32_t palette, float hue)
    {
        uint8_t* words = g_memory.Translate(palette);
        for (int e = 0; e < 256; ++e)
        {
            uint8_t* c = words + e * 4;  // big-endian ARGB
            float r = c[1] / 255.0f, g = c[2] / 255.0f, b = c[3] / 255.0f;
            float mx = std::max({r, g, b}), mn = std::min({r, g, b}), delta = mx - mn;
            if (mx <= 0 || delta / mx <= 0.5f)
                continue;
            float h = mx == r ? 60.0f * std::fmod((g - b) / delta, 6.0f) : mx == g ? 60.0f * ((b - r) / delta + 2) : 60.0f * ((r - g) / delta + 4);
            if (h < 0)
                h += 360;
            if (h < 5 || h > 45)
                continue;
            float sat = delta / mx, v = mx, nh = std::fmod(hue + (h - 25.0f), 360.0f) / 60.0f;
            float x = v * sat * (1 - std::abs(std::fmod(nh, 2.0f) - 1)), cc = v * sat, m = v - cc;
            float rgb[3];
            int sector = int(nh) % 6;
            switch (sector)
            {
            case 0: rgb[0] = cc, rgb[1] = x, rgb[2] = 0; break;
            case 1: rgb[0] = x, rgb[1] = cc, rgb[2] = 0; break;
            case 2: rgb[0] = 0, rgb[1] = cc, rgb[2] = x; break;
            case 3: rgb[0] = 0, rgb[1] = x, rgb[2] = cc; break;
            case 4: rgb[0] = x, rgb[1] = 0, rgb[2] = cc; break;
            default: rgb[0] = cc, rgb[1] = 0, rgb[2] = x; break;
            }
            for (int k = 0; k < 3; ++k)
                c[1 + k] = uint8_t(std::clamp((rgb[k] + m) * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    // Each player keeps its colour on every machine. The game draws player 1 with the Crash template's
    // normal palette and player 2 with its jacked one ("Crash_Player2.bmp"): the character drawable
    // (CCharacterObject: Crash +0xE0 is the CCharacterRenderBehaviour, its +92 the drawable) copies the
    // normal palette of its variant (+4548; palettes at +4516 normal / +4532 jacked) into its base
    // (+348, sub_8222C2D8) at setup, and blends it every frame toward the jacked one by +0x15D0
    // (sub_822212C0; player 2's stays at the jacked end) into the palette it draws with (+0x11C8, 256
    // ARGB words). Here each player's colour follows its slot, fixed (base and blend target the same):
    // slot 2 the jacked palette (pale), the others the normal one, and slots 3 / 4 get the normal one's
    // orange fur turned blue / green after every blend (TintPalette). Done once per drawable (a new
    // level makes new ones).
    void FixLooks(PPCContext& ctx, uint8_t* base, Net& n)
    {
        for (uint32_t i = 0; i < n.players; ++i)
        {
            uint32_t crash = mp::PlayerCrash(i);
            uint32_t render = crash ? LoadBE32(crash + 0xE0) : 0;
            uint32_t d = GuestPointer(render) && LoadBE32(render) == 0x82015BC4 ? LoadBE32(render + 92) : 0;
            if (!GuestPointer(d) || !mp::PlayerOnFoot(i) || d == n.lookDone[i])
                continue;
            uint32_t variant = LoadBE32(d + 4548) & 3;
            uint32_t normal = LoadBE32(d + 4516 + variant * 4), jacked = LoadBE32(d + 4532 + variant * 4);
            if (!GuestPointer(normal) || !GuestPointer(jacked))
                continue;
            uint32_t slot = SlotOf(n.slot, i);
            bool pale = slot == 1;
            uint32_t look = pale ? jacked : normal;
            PPCContext call = ctx;
            call.r3.u64 = d + 348;
            call.r4.u64 = look + 16;
            sub_8222C2D8(call, base);
            StoreBE32(d + 4532 + variant * 4, look);
            n.lookDone[i] = d;
            {
                std::lock_guard<std::mutex> lock(TintMutex());
                Tints()[d] = slot == 2 ? 215.0f : slot == 3 ? 115.0f : -1.0f;
            }
            static const char* names[] = {"orange", "pale (jacked)", "blue", "green"};
            Log("[mp] player %u (slot %u): %s", i + 1, slot + 1, names[slot & 3]);
        }
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
        for (uint32_t i = 0; i < 4; ++i)
            if (mp::PlayerCrash(i) && PlayerActor(i) == actor)
            {
                if (!mp::PlayerOnFoot(i))
                    r.flags |= 2;
                // A player's hitpoints go as a fraction of its maximum (machines may have different ones).
                float max = mp::PlayerMaxHitpoints(i), hp = mp::PlayerHitpoints(i);
                r.hitpoints = max > 0 && hp >= 0 ? hp / max : -1.0f;
            }
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

    // The host sends to every client, a client to the host; `except` is a slot left out.
    void SendToPeers(Net& n, const uint8_t* data, size_t size, uint32_t except = ~0u)
    {
        if (n.role == Role::Client)
        {
            sendto(n.sock, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&n.peer),
                   sizeof(n.peer));
            return;
        }
        sockaddr_in to[4];
        {
            std::lock_guard<std::mutex> lock(n.padMutex);
            memcpy(to, n.clients, sizeof(to));
        }
        for (uint32_t k = 1; k < n.players; ++k)
            if (k != except)
                sendto(n.sock, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&to[k]),
                       sizeof(to[k]));
    }

    void Flush(Net& n, std::vector<uint8_t>& packet, uint32_t count)
    {
        if (!count)
            return;
        packet[8] = uint8_t(count >> 24);
        packet[9] = uint8_t(count >> 16);
        packet[10] = uint8_t(count >> 8);
        packet[11] = uint8_t(count);
        SendToPeers(n, packet.data(), packet.size());
        n.bytes += (packet.size() + 28) * (n.role == Role::Host ? n.players - 1 : 1);  // + IPv4 and UDP headers
        n.packets++;
    }

    // The peer's player pad ("CMI1", sequence, the 12-byte XINPUT_GAMEPAD as its game read it): the
    // client's on the host, the host's player 1 on the client.
    // A player's pad ("CMI2", its slot, sequence, the 12-byte XINPUT_GAMEPAD as its game read it). The
    // host learns the client's address from it and passes it on to the other clients.
    void ReceivePad(Net& n, const uint8_t* buffer, int got, const sockaddr_in& from)
    {
        if (got < 20 + int(sizeof(XINPUT_GAMEPAD)) || Get32(buffer) != INPUT_MAGIC)
            return;
        uint32_t slot = Get32(buffer + 4), sequence = Get32(buffer + 8);
        if (slot >= n.players || slot == n.slot)
            return;
        {
            std::lock_guard<std::mutex> lock(n.padMutex);
            if (sequence <= n.padSequence[slot] && n.padSequence[slot] - sequence < 1000)
                return;  // late or duplicated
            n.padSequence[slot] = sequence;
            uint16_t before = n.remotePad[slot].wButtons;
            memcpy(&n.remotePad[slot], buffer + 12, sizeof(XINPUT_GAMEPAD));
            for (int i = 0; i < 2; ++i)
            {
                uint32_t bits = Get32(buffer + 12 + sizeof(XINPUT_GAMEPAD) + i * 4);
                memcpy(&n.remoteForward[slot][i], &bits, 4);
            }
            if (mp::Trace() && n.remotePad[slot].wButtons != before)
                Log("[mp] pad in t=%llu slot %u buttons %04X", (unsigned long long)mp::NowMs(), slot + 1,
                    n.remotePad[slot].wButtons);
            n.remotePadAt[slot] = mp::NowMs();
            n.padPackets++;
            if (n.role == Role::Host)
                n.clients[slot] = from;
        }
        if (n.role == Role::Host)
            SendToPeers(n, buffer, size_t(got), slot);
    }

    void HostReceive(Net& n)
    {
        uint8_t buffer[256];
        for (;;)
        {
            sockaddr_in from{};
            socklen_t fromSize = sizeof(from);
            int got = int(recvfrom(n.sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromSize));
            if (got <= 0)
                break;
            ReceivePad(n, buffer, got, from);
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
                n.padPackets / seconds, n.remotePad[1].wButtons);
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
            sockaddr_in from{};
            socklen_t fromSize = sizeof(from);
            int got = int(recvfrom(n.sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromSize));
            if (got <= 0)
                break;
            if (got < int(HEADER) || Get32(buffer) != MAGIC)
            {
                ReceivePad(n, buffer, got, from);
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
        // The client plays its own slot as player 1: the host's record of player index g (slot g) goes on
        // this machine's player LocalIndex(slot, g); the players have the same names everywhere.
        uint32_t players[4];
        uint64_t playerNames[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            players[i] = i < n.players ? PlayerActor(i) : 0;
            playerNames[i] = players[i] ? NameOf(players[i]) : 0;
        }
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
        } targets[4];
        for (auto& [recordName, r] : n.latest)
        {
            uint64_t name = recordName;
            int player = -1;  // local index of the player this record drives
            for (uint32_t g = 0; g < n.players; ++g)
                if (playerNames[g] && name == playerNames[g])
                {
                    player = int(LocalIndex(n.slot, g));
                    name = playerNames[player];
                    break;
                }
            if (player < 0 && r.flags & 1)
                continue;  // a player this machine has not spawned (yet)
            auto it = n.local.find(name);
            if (it == n.local.end())
            {
                n.missing++;
                continue;
            }
            uint32_t xform = LoadBE32(it->second + 44);
            if (!xform)
                continue;
            if (player >= 0)
            {
                // A player not on foot (not shown yet) is the game's to move.
                if (!mp::PlayerOnFoot(uint32_t(player)))
                    continue;
                float dx = LoadF(xform + 48) - r.m[9], dy = LoadF(xform + 52) - r.m[10], dz = LoadF(xform + 56) - r.m[11];
                uint32_t index = uint32_t(player);
                // At most every 250 ms: a player that cannot get there (pushed, hit) is not teleported every frame.
                // Moving, only past `snap` (a correction shows as a jump); standing (the host's record still for
                // 0.5 s, the player here still too), anything past 0.25, so no offset stays for good.
                uint64_t now = mp::NowMs();
                float* last = n.lastRecordPos[index];
                if (std::abs(last[0] - r.m[9]) + std::abs(last[1] - r.m[10]) + std::abs(last[2] - r.m[11]) > 0.05f)
                {
                    last[0] = r.m[9], last[1] = r.m[10], last[2] = r.m[11];
                    n.recordMovedAt[index] = now;
                }
                float* mine = n.lastLocalPos[index];
                float here[3] = {LoadF(xform + 48), LoadF(xform + 52), LoadF(xform + 56)};
                if (std::abs(mine[0] - here[0]) + std::abs(mine[1] - here[1]) + std::abs(mine[2] - here[2]) > 0.05f)
                {
                    memcpy(mine, here, sizeof(here));
                    n.localMovedAt[index] = now;
                }
                bool still = now - n.recordMovedAt[index] >= 500 && now - n.localMovedAt[index] >= 500;
                // Pushing against another player (within 1.6), each machine sees the push a few frames
                // apart: up to 2.0 is let go while moving (the correction standing still evens it out).
                bool contact = false;
                for (uint32_t j = 0; j < n.players && !contact; ++j)
                    if (uint32_t other = j != index ? PlayerActor(j) : 0; other && GuestPointer(LoadBE32(other + 44)))
                    {
                        uint32_t ox = LoadBE32(other + 44);
                        float cx = LoadF(ox + 48) - here[0], cz = LoadF(ox + 56) - here[2];
                        contact = cx * cx + cz * cz < 1.6f * 1.6f;
                    }
                float limit = still ? 0.25f : contact ? std::max(n.snap, 2.0f) : n.snap;
                // A knocked-out player lies where it fell and comes back at the same place on every machine.
                // A correction that does not hold (the place is taken here, or the player slides off it) is tried
                // 3 times, then left until the host's record moves.
                float* prev = n.lastSnapPos[index];
                bool same = std::abs(prev[0] - r.m[9]) + std::abs(prev[1] - r.m[10]) + std::abs(prev[2] - r.m[11]) < 0.1f;
                if (!(r.flags & 2) && !mp::HitpointsHeld(index) && dx * dx + dy * dy + dz * dz > limit * limit &&
                    now - n.lastSnap[index] >= 250 && !(same && n.snapRepeats[index] >= 3))
                {
                    targets[index] = {it->second, {r.m[9], r.m[10], r.m[11]}};
                    n.lastSnap[index] = mp::NowMs();
                    n.snapRepeats[index] = same ? n.snapRepeats[index] + 1 : 1;
                    prev[0] = r.m[9], prev[1] = r.m[10], prev[2] = r.m[11];
                    if (mp::Trace())
                        Log("[mp] P%u off by %.2f (%.1f, %.1f, %.1f here)", index + 1, std::sqrt(dx * dx + dy * dy + dz * dz),
                            here[0], here[1], here[2]);
                }
                // The players' hitpoints are the host's too (so a knock-out comes on every machine), except
                // while one is knocked out here or just back (the host's record may still say 0).
                n.recordHitpoints[index] = r.hitpoints;
                if (!mp::HitpointsHeld(index))
                    ApplyHitpoints(ctx, base, n, it->second, r, name, true);
                continue;
            }
            for (int row = 0; row < 4; ++row)
                for (int c = 0; c < 3; ++c)
                    StoreF(xform + row * 16 + c * 4, r.m[row * 3 + c]);
            n.applied++;
            ApplyHitpoints(ctx, base, n, it->second, r, name);
        }
        for (const Target& t : targets)
            if (t.actor && TeleportPlayer(ctx, base, t.actor, t.pos[0], t.pos[1], t.pos[2]))
            {
                n.teleports++;
                if (mp::Trace())
                    Log("[mp] snap P%u to (%.1f, %.1f, %.1f)", uint32_t(&t - targets) + 1, t.pos[0], t.pos[1], t.pos[2]);
            }
        if (mp::Trace() && mp::NowMs() - n.lastSyncTrace >= 250)
        {
            n.lastSyncTrace = mp::NowMs();
            TracePlayers("client", n.lastSyncTrace);
            TraceEnemies("client", n.lastSyncTrace);
        }
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

    uint32_t PlayerSlot(uint32_t index) { return Online() ? SlotOf(N().slot, index) : index; }

    float PlayerHitpoints(uint32_t index)
    {
        uint32_t damageable = PlayerDamageable(N(), PlayerActor(index));
        return damageable ? LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS)) : -1.0f;
    }

    void SetPlayerMaxHitpoints(uint32_t index, float value)
    {
        uint32_t damageable = PlayerDamageable(N(), PlayerActor(index));
        uint32_t max = damageable ? LoadBE32(damageable + 68) : 0, hp = damageable ? LoadBE32(damageable + DAMAGEABLE_HITPOINTS) : 0;
        if (!GuestPointer(max) || !GuestPointer(hp) || value <= 0)
            return;
        StoreF(max, value);
        if (LoadF(hp) > value)
            StoreF(hp, value);
    }

    float PlayerMaxHitpoints(uint32_t index)
    {
        uint32_t damageable = PlayerDamageable(N(), PlayerActor(index));
        uint32_t max = damageable ? LoadBE32(damageable + 68) : 0;
        return GuestPointer(max) ? LoadF(max) : -1.0f;
    }

    bool TeleportCrash(PPCContext& ctx, uint8_t* base, uint32_t crash, float x, float y, float z)
    {
        uint32_t physics = GuestPointer(crash) ? LoadBE32(crash + 0xDC) : 0;
        uint32_t actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
        if (!GuestPointer(actor))
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

    double Hitpoints(uint32_t damageable, double delta)
    {
        if (!Enabled())
            return delta;
        Net& n = N();
        uint32_t actor = LoadBE32(damageable + BEHAVIOUR_ACTOR);
        if (!GuestPointer(actor))
            return delta;
        n.damageables[actor] = damageable;
        if (n.role == Role::None)
            return delta;
        if (Trace() && delta < 0)
            Log("[mp] %s: %08X takes %.1f (hitpoints %.1f)%s", n.role == Role::Host ? "host" : "client", uint32_t(NameOf(actor)),
                -delta, LoadF(LoadBE32(damageable + DAMAGEABLE_HITPOINTS)), n.applyingHitpoints ? " from the host" : "");
        if (n.role != Role::Client || delta >= 0 || n.applyingHitpoints)
            return delta;
        // A player the host mirrors, or an actor, takes its hits from the host's records only (a knocked-out
        // player, or one just back, keeps this machine's).
        for (uint32_t i = 0; i < 4; ++i)
            if (PlayerActor(i) == actor && HitpointsHeld(i))
                return delta;
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
        Put32(packet, n.slot);
        Put32(packet, uint32_t(++n.sentPadSequence));
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&pad);
        packet.insert(packet.end(), raw, raw + sizeof(pad));
        float forward[2] = {0, 0};
        CameraForward(forward[0], forward[1]);
        for (float f : forward)
        {
            uint32_t bits;
            memcpy(&bits, &f, 4);
            Put32(packet, bits);
        }
        SendToPeers(n, packet.data(), packet.size());
    }

    bool RemotePad(uint32_t user, void* pad)
    {
        if (!Enabled())
            return false;
        Net& n = N();
        // Players 2-4 are shown on foot at the start (mp_players.cpp): controller `user` stays plugged in,
        // on the pad of the slot it plays; a player that went quiet leaves its Crash standing.
        if (n.role == Role::None || user == 0 || user >= n.players || !InLevel())
            return false;
        uint32_t slot = SlotOf(n.slot, user);
        XINPUT_GAMEPAD& game = *static_cast<XINPUT_GAMEPAD*>(pad);
        game = {};
        {
            std::lock_guard<std::mutex> lock(n.padMutex);
            if (n.remotePadAt[slot] && NowMs() - n.remotePadAt[slot] <= 2000)
                memcpy(pad, &n.remotePad[slot], sizeof(XINPUT_GAMEPAD));
            // The left stick as the sender meant it in the world: from its camera's axes to this one's.
            float fx = n.remoteForward[slot][0], fz = n.remoteForward[slot][1], gx, gz;
            if ((fx || fz) && CameraForward(gx, gz) && (game.sThumbLX || game.sThumbLY))
            {
                float sx = game.sThumbLX, sy = game.sThumbLY;
                float wx = sx * fz + sy * fx, wz = -sx * fx + sy * fz;  // stick.x * right + stick.y * forward
                float rx = wx * gz - wz * gx, ry = wx * gx + wz * gz;   // onto this camera's right / forward
                game.sThumbLX = int16_t(std::clamp(rx, -32768.0f, 32767.0f));
                game.sThumbLY = int16_t(std::clamp(ry, -32768.0f, 32767.0f));
                static uint64_t traced[4];
                if (Trace() && NowMs() - traced[slot] >= 400)
                {
                    traced[slot] = NowMs();
                    Log("[mp] stick slot %u: (%.0f, %.0f) camera (%.2f, %.2f) -> (%d, %d) camera (%.2f, %.2f)", slot + 1, sx, sy,
                        fx, fz, game.sThumbLX, game.sThumbLY, gx, gz);
                }
            }
        }
        return true;
    }

    void NetTick(PPCContext& ctx, uint8_t* base)
    {
        Net& n = N();
        if (n.role != Role::None && InLevel())
            FixLooks(ctx, base, n);
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

// After the game blends a character's palette for the frame: the players of slots 3 / 4 get their fur
// colour (FixLooks).
PPC_FUNC(sub_822212C0)
{
    uint32_t drawable = ctx.r3.u32;
    __imp__sub_822212C0(ctx, base);
    if (!mp::Enabled())
        return;
    float hue = -1;
    {
        std::lock_guard<std::mutex> lock(TintMutex());
        auto it = Tints().find(drawable);
        if (it != Tints().end())
            hue = it->second;
    }
    if (hue < 0)
        return;
    // Still a player's (an old level's drawable may have been freed and its memory reused).
    for (uint32_t i = 0; i < 4; ++i)
        if (uint32_t crash = mp::PlayerCrash(i))
            if (uint32_t render = LoadBE32(crash + 0xE0); GuestPointer(render) && LoadBE32(render + 92) == drawable)
            {
                TintPalette(drawable + 0x11C8, hue);
                return;
            }
}
