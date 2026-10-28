// Online multiplayer prototype: the level streaming (docs/internals/multiplayer.md, runtime/NOTES.md
// "Multijogador online (protótipo)"). Nothing here runs unless CRASH_MP=1.
//
// How the game streams a level (CStreamManager, the ":Streamer" object):
//
//   - The stream manager is game manager 17 (sub_821FA440(managers, 17); its Update sub_82250510
//     runs every frame). Not *0x824A90CC, which is the package manager.
//   - levels\<L>\streamgraph.blua builds the level's rooms: streamer:AddRoom(name) (sub_8224ED20,
//     a 600-byte room, list at sm +20..+24), one convex hull per room (AddHull / AddFace), its world
//     package (AddResource("L1_E1_R1.p3d", EManager_WORLD)) and its adjacent rooms
//     (AddAdjacentRoom; the list at room +312..+316). 1 to 10 rooms per level, all "Tiny" or "Light".
//   - sm +636 is the current room, sm +640 the state (0..3, Update sub_82250510). The room change
//     check sub_8224E760 looks only at the two players (CGameDatabase +16 / +20, and the actors at
//     +24 / +28): the current room is kept while ANY of them is inside it; when nobody is, the new
//     current room is player 1's (sub_8224E6F8: the room whose hull holds actor +44 -> +48).
//   - The resident set is the current room plus its adjacent rooms (sub_82250200): a sorted vector
//     of room pointers, double-buffered at sm +700 / +704 (capacity 10). Against the previous set
//     it makes the keep list (+684 = old & new), the unload list (+668 = old - keep) and the load
//     list (+652 = new - keep); states 2 / 3 then unload and load the rooms' packages.
//   - CStreamInterestBehaviour (every Crash has one) only registers the actor's position in a list
//     of 4 at sm +768 (sub_8224F2D8 / sub_8224E880); the room choice above does not read it.
//
// CRASH_MP_STREAM picks the resident set:
//   vanilla  the game's (current room + adjacent rooms)
//   union    the current room, every player's room, and the rooms adjacent to each of them
//   all      every room of the level stays loaded
// With the prototype on, the room change check is the game's rule for any number of players: the
// current room stays while any player (or CRASH_MP_ANCHOR position, a stand-in for a remote player:
// "x,y,z;x,y,z") is inside it, or outside every room; otherwise the first player's room.
//
// CRASH_MP_TRACE=1 logs the current room, the resident set and the players' rooms once a second
// ("[mp] stream ...").

#include "mp.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82250200);  // CStreamManager: resident set for room r4
extern "C" PPC_FUNC(__imp__sub_8224ED20);  // CStreamManager::AddRoom(name)
extern "C" PPC_FUNC(__imp__sub_82250510);  // CStreamManager::Update
extern "C" PPC_FUNC(__imp__sub_8224E760);  // CStreamManager: room change check (0 = keep the current room)
PPC_EXTERN_FUNC(sub_8224EA30);             // sorted intersection (out, a, b)
PPC_EXTERN_FUNC(sub_8224E968);             // sorted difference (out, a, b)
PPC_EXTERN_FUNC(sub_8224E520);             // room whose hull holds a position (sm, pos, hint)

namespace
{
    uint32_t g_streamManager = 0;  // seen by the Update / resident set hooks

    std::map<uint32_t, std::string> g_roomNames;  // room -> its streamgraph name (AddRoom hook)
    uint64_t g_lastTrace = 0;

    std::string RoomName(uint32_t room)
    {
        if (!room)
            return "-";
        auto it = g_roomNames.find(room);
        if (it != g_roomNames.end())
            return it->second;
        char text[16];
        snprintf(text, sizeof(text), "%08X", room);
        return text;
    }

    std::vector<uint32_t> ReadVector(uint32_t vec)  // {begin, end, capacity} of guest pointers
    {
        std::vector<uint32_t> out;
        for (uint32_t p = LoadBE32(vec); p && p < LoadBE32(vec + 4); p += 4)
            out.push_back(LoadBE32(p));
        return out;
    }

    std::vector<uint32_t> Rooms(uint32_t sm) { return ReadVector(sm + 20); }
    std::vector<uint32_t> Adjacent(uint32_t room) { return ReadVector(room + 312); }

    std::string Names(const std::vector<uint32_t>& rooms)
    {
        std::string out;
        for (uint32_t r : rooms)
            out += (out.empty() ? "" : ",") + RoomName(r);
        return out;
    }
}

namespace mp
{
    uint32_t StreamManager() { return g_streamManager; }

    uint32_t RoomAt(PPCContext& ctx, uint8_t* base, uint32_t position)
    {
        uint32_t sm = StreamManager();
        // Before the first room change the hulls may not be there yet.
        if (!sm || !position || !LoadBE32(sm + 636))
            return 0;
        PPCContext call = ctx;
        call.r3.u64 = sm;
        call.r4.u64 = position;
        call.r5.u64 = 0;
        sub_8224E520(call, base);
        return call.r3.u32;
    }

    std::string RoomLabel(uint32_t room) { return RoomName(room); }
}

// AddRoom: remembers each room's name for the trace.
PPC_FUNC(sub_8224ED20)
{
    std::string name;
    if (mp::Enabled() && ctx.r5.u32)
        name = reinterpret_cast<const char*>(g_memory.Translate(ctx.r5.u32));
    uint32_t sm = ctx.r4.u32;
    __imp__sub_8224ED20(ctx, base);
    if (!name.empty() && sm)
    {
        std::vector<uint32_t> rooms = Rooms(sm);
        if (!rooms.empty())
            g_roomNames[rooms.back()] = name;
    }
}

// The resident set for the current room: the game's, then (union / all) widened and the keep /
// unload / load lists made again from it, as the original does.
PPC_FUNC(sub_82250200)
{
    uint32_t sm = ctx.r3.u32;
    uint32_t room = ctx.r4.u32;
    g_streamManager = sm;
    __imp__sub_82250200(ctx, base);
    mp::StreamMode mode = mp::Stream();
    if (!mp::Enabled() || mode == mp::StreamMode::Vanilla)
        return;

    std::vector<uint32_t> set = ReadVector(LoadBE32(sm + 704));
    auto add = [&](uint32_t r) {
        if (r && std::find(set.begin(), set.end(), r) == set.end())
            set.push_back(r);
    };
    if (mode == mp::StreamMode::All)
    {
        for (uint32_t r : Rooms(sm))
            add(r);
    }
    else
    {
        for (uint32_t actor : mp::PlayerActors())
        {
            uint32_t xform = LoadBE32(actor + 44);
            uint32_t r = xform ? mp::RoomAt(ctx, base, xform + 48) : 0;
            if (!r)
                continue;
            add(r);
            for (uint32_t a : Adjacent(r))
                add(a);
        }
    }
    uint32_t vec = LoadBE32(sm + 704);
    uint32_t begin = LoadBE32(vec), capacity = (LoadBE32(vec + 8) - begin) / 4;
    std::sort(set.begin(), set.end());
    if (set.size() > capacity)
    {
        Log("[mp] stream: %zu rooms wanted, room set holds %u; keeping the first ones", set.size(), capacity);
        set.resize(capacity);
    }
    for (size_t i = 0; i < set.size(); ++i)
        StoreBE32(begin + uint32_t(i) * 4, set[i]);
    StoreBE32(vec + 4, begin + uint32_t(set.size()) * 4);

    PPCContext call = ctx;
    call.r3.u64 = sm + 684;
    call.r4.u64 = LoadBE32(sm + 700);
    call.r5.u64 = vec;
    sub_8224EA30(call, base);
    call = ctx;
    call.r3.u64 = sm + 668;
    call.r4.u64 = LoadBE32(sm + 700);
    call.r5.u64 = sm + 684;
    sub_8224E968(call, base);
    call = ctx;
    call.r3.u64 = sm + 652;
    call.r4.u64 = vec;
    call.r5.u64 = sm + 684;
    sub_8224E968(call, base);
    (void)room;
}

// The room change check for every player (see the top of the file). Returns the new room, or 0.
PPC_FUNC(sub_8224E760)
{
    if (!mp::Enabled())
    {
        __imp__sub_8224E760(ctx, base);
        return;
    }
    uint32_t sm = ctx.r3.u32;
    uint32_t current = LoadBE32(sm + 636);
    std::vector<uint32_t> positions;
    for (uint32_t actor : mp::PlayerActors())
        if (uint32_t xform = LoadBE32(actor + 44))
            positions.push_back(xform + 48);
    for (uint32_t anchor : mp::Anchors())
        positions.push_back(anchor);
    uint32_t first = 0;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        uint32_t room = mp::RoomAt(ctx, base, positions[i]);
        if (!room)
            room = current;  // outside every hull: as the game, counts as the current room
        if (i == 0)
            first = room;
        if (room == current)
        {
            ctx.r3.u64 = 0;
            return;
        }
    }
    *g_memory.Translate(sm + 649) = 0;
    ctx.r3.u64 = first;
}

// Update: the once-a-second trace, and the prototype's per-frame tick (the stream manager runs
// every frame of a level, on the game thread).
PPC_FUNC(sub_82250510)
{
    uint32_t sm = ctx.r3.u32;
    g_streamManager = sm;
    __imp__sub_82250510(ctx, base);
    if (!mp::Enabled())
        return;
    mp::Tick(ctx, base);
    if (!mp::Trace())
        return;

    // Every frame: a player that moved more than 10 units since the last frame (a teleport, the
    // game's reset to the last checkpoint locator) is logged with both positions.
    static std::map<uint32_t, std::array<float, 3>> last;
    for (uint32_t actor : mp::PlayerActors())
    {
        uint32_t xform = LoadBE32(actor + 44);
        if (!xform)
            continue;
        std::array<float, 3> p{};
        for (int i = 0; i < 3; ++i)
        {
            uint32_t bits = LoadBE32(xform + 48 + i * 4);
            memcpy(&p[i], &bits, 4);
        }
        auto it = last.find(actor);
        if (it != last.end())
        {
            float dx = p[0] - it->second[0], dy = p[1] - it->second[1], dz = p[2] - it->second[2];
            if (dx * dx + dy * dy + dz * dz > 100.0f)
                Log("[mp] actor %08X jumped (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) room %s", actor, it->second[0],
                    it->second[1], it->second[2], p[0], p[1], p[2], RoomName(mp::RoomAt(ctx, base, xform + 48)).c_str());
        }
        last[actor] = p;
    }

    uint64_t now = mp::NowMs();
    if (now - g_lastTrace < mp::TraceIntervalMs())
        return;
    g_lastTrace = now;

    std::string players;
    int index = 1;
    for (uint32_t actor : mp::PlayerActors())
    {
        uint32_t xform = LoadBE32(actor + 44);
        float p[3]{};
        if (xform)
            for (int i = 0; i < 3; ++i)
            {
                uint32_t bits = LoadBE32(xform + 48 + i * 4);
                memcpy(&p[i], &bits, 4);
            }
        char text[160];
        snprintf(text, sizeof(text), " P%d[%08X](%.1f,%.1f,%.1f)@%s", index++, actor, p[0], p[1], p[2],
                 xform ? RoomName(mp::RoomAt(ctx, base, xform + 48)).c_str() : "-");
        players += text;
    }
    Log("[mp] stream state %u current %s resident [%s] loading [%s] unloading [%s]%s", LoadBE32(sm + 640),
        RoomName(LoadBE32(sm + 636)).c_str(), Names(ReadVector(LoadBE32(sm + 700))).c_str(),
        Names(ReadVector(sm + 652)).c_str(), Names(ReadVector(sm + 668)).c_str(), players.c_str());
}
