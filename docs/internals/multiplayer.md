# Online multiplayer (prototype, M0 / M1 / M2 / streaming)

Feasibility work for an online mode of up to 5 players (branch `online-coop`, 2026-10-28). Local co-op stays
the game's own 2 players. Full write-up in Portuguese: `runtime/NOTES.md`, "Multijogador online
(protótipo)". Code: `runtime/online/` (`mp.h`). **Everything is off unless `CRASH_MP=1`**; with it off no
hook changes anything.

| Switch | What |
|---|---|
| `CRASH_MP=1` | turns the prototype on |
| `CRASH_MP_STREAM=vanilla\|union\|all` | resident rooms (default vanilla = the game's) |
| `CRASH_MP_ANCHOR=x,y,z;...` | positions that count as players in the room choice (stand-ins for remote players) |
| `CRASH_MP_TRACE=1\|<ms>` | rooms + players once a second, every player move > 10 units in one frame |
| `CRASH_MP_ROLE=host\|client` | the M1 UDP mirror; `CRASH_MP_PEER` (127.0.0.1), `CRASH_MP_PORT` (47810; client = +1), `CRASH_MP_HZ` (30) |
| `CRASH_MP_SUPPRESS=ai,objectives` | client: skip CAIManager / CObjectiveManager updates once its level is up |
| `CRASH_MP_REMOTE_USER=<n>` | the controller the peer's pad becomes (default 1 = the game's player 2) |
| `CRASH_MP_SNAP=<units>` | client: a player further than this from the host's record is put back through its physics (default 1.0) |

Profiles (`test = false`): `mp_stream_vanilla`, `mp_stream_union`, `mp_stream_all`, `mp_host` + `mp_client`
(run together; windows side by side through `CRASH_RESTART_WINDOW`), `mp_tres_crash`, `mp_m2_host` + `mp_m2_client`,
`mp_marionete`.

## Level streaming (the user's must-have question)

- The stream manager is **game manager 17** (`sub_821FA440(managers, 17)`, Lua `":Streamer"`), Update
  `sub_82250510`. `*0x824A90CC` is the package manager, not it (reading rooms through it crashed).
- `levels\<L>\streamgraph.blua` builds **rooms**: `AddRoom(name)` `sub_8224ED20` (600-byte room, list at
  sm +20..+24), convex hulls (`AddHull` / `AddFace`, room +280..+284), one world package
  (`AddResource("L1_E1_R1.p3d", EManager_WORLD)`), adjacency (`AddAdjacentRoom`, room +312..+316, one-way
  lists). 1 to 10 rooms per level, all `Tiny` or `Light`. Load state at room +392.
- sm +636 = current room, sm +640 = state (1 = idle / checking, 2 / 3 = unloading / loading).
- **Room change check** `sub_8224E760`: looks only at CGameDatabase +16 / +20 (the two players) and +24 / +28.
  The current room stays while ANY of them is inside it (`sub_8224E6F8`: hull test `sub_8224E520` /
  `sub_822569E8` on actor +44 -> +48; outside every hull counts as the current room); when nobody is, the new
  current room is player 1's.
- **Resident set** `sub_82250200(sm, room)`: room + its adjacent rooms, sorted vector of room pointers,
  double-buffered at sm +700 / +704 (capacity 10 = the largest level). Diffs: keep +684 = old & new, unload
  +668 = old - keep, load +652 = new - keep (`sub_8224EA30` intersection, `sub_8224E968` difference).
- Actors belong to a room (actor +40; room actor list +344..+348; global room at sm +36). An actor whose room
  is not resident is **deactivated** (`sub_820906E8` -> `sub_82256180` -> `sub_820903A8` / `sub_82090450`):
  enemies in an unloaded room stop.
- `CStreamInterestBehaviour` (every Crash) only registers its position in a list of 4 at sm +768
  (`sub_8224F2D8` / `sub_8224E880`); the room choice does not read it.
- A player outside the resident rooms has no floor: they fall and the game puts them back at the last
  `Whumpa...ResetLocator` (seen: P1 at a hole -> y -41 -> reset). `PhysicsBehaviour:Teleport` into a room that
  is not resident does nothing (the actor stays where it was).

Tests (L1_E1, an anchor in R1 standing for a remote player, P1 teleported R2 -> R4 -> R5 on floor points):

| Policy | Result |
|---|---|
| vanilla | current room stays R1 (anchor); resident R1 + R2; the teleport to R4 is refused, P1 stays in R2 |
| union | P1 reaches R4 and R5 and stands on the floor; resident = R1, R2 (anchor) + R5, R6 (P1), renders fine |
| all | all 8 rooms resident at once, no crash / out of memory; also L2_E1 (10 rooms, the max) and L5_E2 (4 Light + 3 Tiny) |

**Recommended policy:** the host keeps the **union** (every player's room + its adjacent rooms; `all` fits in
memory for the levels tried, so it is the safe fallback). The current room (fog, sky, ocean) stays the
game's choice. A client streams around **its own** player (vanilla on its machine) and should skip the
host's records for actors in rooms it has not loaded (not done in the prototype). Leash only where the level needs everyone together:
cinematics, level end, `DO_TeleportAllPlayers` (native), and as a safety net a teleport-to-nearest-player
when someone is outside every hull or falls (the game's own reset locators already catch falls). A client
whose player is in a room the host would unload cannot happen with union: the host keeps it loaded, so the
enemies there keep running.

## Players (M0)

- Crash's constructor `sub_82153860`: player index byte at bc +292 = first free CGameDatabase slot
  (+16, +20); stores bc in `0x8249D778[index]` (2 entries). A **third Crash** gets index 2 and its store
  lands on `0x8249D780`, the co-op "backpack" Crash pointer (`EnterOtherPlayersBackpack` `sub_82154D10`,
  `IsOkayToLeaveBackpack` `sub_821544E0`). The prototype hook restores that word.
- Both players are spawned at level start (`objectives.lua`: `DO_SpawnPlayer(1, "ref_Crash", ...)`,
  `DO_SpawnPlayer(2, "ref_Crash_P2", ...)`); an unjoined P2 rides along with P1. Lua `GetCurrentPlayer(1)`
  returns P1.
- `DO_SpawnPlayer(3, "ref_Crash_P3", "PlayerSpawnPosition")` builds the third Crash (index 2) and the game
  **crashes ~2 s later** (null read in the Lua VM, `sub_8229FFD8`). The 2-player limit is everywhere:
  `index < 2` checks (GetCurrentPlayer, room check, Crash::vf20...), 2-entry tables, CGameDatabase's 2 + 2
  slots, `CPlayerOneHealthDisplay` / `CPlayerTwoHealthDisplay`, the backpack, "P2 PAUSED".
- Input: Crash's input map at bc +176 -> device at +0x1FC (CInputManager = manager 7, devices at +8..+12).
- Colour: P2's look is the template's `jacked_skin_1` texture `Crash_Player2.bmp` (`normal_skin_1` is
  `crash_normal.bmp`); the render behaviour has 4 normal / 4 jacked slots, palettes per drawable variant at
  +4516 / +4532 (see `settings/snipe_colors.cpp`). 5 colours = recoloured palettes at load time, as the
  blue Snipe does.

## Actors and the mirror (M1)

- Actor name = 64-bit hash at actor +8 (`newActor`'s name, the same on every machine): the network id.
- actor +44 -> transform: 4x4 matrix, rows 0-2 = axes, row 3 (+48) = position.
- `CActor::GetBehaviour(actor, type)` `sub_82142608`, type = the class's vf05: 2 fight tree
  (`CCharacterFightTreeBehaviour` vtable 0x82010474; +28 -> state: +132 state time, +136 sub-state, +140
  general), 3 physics, 38 render. Validate the vtable: cinematic actors return other objects.
- Per-frame updates (CManager::vf10): `CActorManager` `sub_82092668` (actors), `CAIManager` `sub_82099058`,
  `CObjectiveManager` `sub_8220AC38`. The client writes transforms right after the actor update and can skip
  AI and objectives.
- Result (two instances on one PC, L1_E1): the client mirrored 439-447 actors, 0 packets lost, its Crash at
  the host's position in R2. Host-spawned enemies (objectives) don't exist on the client: M3 needs spawn
  events. Pose/animation is still the client's own.
- Bandwidth at 30 Hz, sending only changed actors (+ all once a second): ~1,750 records/s (~58 per
  snapshot: ambient props animate), 110 packets/s, **~114 KB/s raw (64-byte records, ~0.9 Mbit/s)**,
  ~45 KB/s with a compact record (16-bit id, quantised position + quaternion, fight node + time:
  ~24 bytes). Players only: 5 x 30 Hz x 24 B = ~3.6 KB/s. Interest management (only actors near any player,
  only moving ones, 20 Hz for far ones) cuts the world part further.

## Client input on the host (M2)

The first M2 route uses the game's own player 2 instead of a new puppet class, so nothing new has to be
spawned and every native player path (camera, backpack, damage, triggers) already works for it:

- The client sends player 1's pad as the game read it (`XamInputGetState` path in
  `kernel/xam_input_imports.cpp`): "CMI1" packets, sequence + the 12-byte `XINPUT_GAMEPAD`, on change and at
  least every 50 ms (~19 packets/s idle). Only inside a level (`mp::InLevel()`: stream manager has a current
  room and CGameDatabase +16 is set), else the host's title screen takes controller 1 as player 1.
- The host's `ReadController` for user `CRASH_MP_REMOTE_USER` returns that pad (connected) while packets
  arrive (2 s timeout = unplugged). Late / duplicated sequences are dropped.
- In the game P2 joins with **B / Circle** (into P1's backpack) and the **same button** leaves it; Y
  (Triangle) does not.
- **Both machines open the level with the two Crashes on foot**, the way the game does between two co-op
  levels: `Crash::Crash` copies the co-op state the last level left (`0x8245FA9C[index]`: 0 = not joined,
  1 = in the backpack, 2 = on foot; `Crash::vf20` saves it) into Crash +300, and `0x8245FA98` is the player
  that auto-joins into the backpack at the start (`OtherPlayerAutoJoinAtStart` `sub_82163A40` ->
  join `sub_82154110(player, controller)`: flags +0x110 |= 0x18, objective event 0x2F). Online, before the
  constructors: P2 state 0, auto-join = 1; then while P2 rides (Crash +300 = 1) its controller taps B, so it
  is off ~20 ms later, under the episode title. Setting state 2 directly skips P2's model setup: only one
  Crash is drawn (P2's model sits on P1).
- Online nobody is pulled into the backpack afterwards: co-op puts the player the shared camera leaves behind
  into the other one's (`EnterOtherPlayersBackpack` `sub_82154D10` on a Crash already on foot); each
  machine has its own camera, so the hook skips it (the start join, state 0, still goes through).
- **Animation by input mirroring.** Both machines send their player 1's pad ("CMI1", without START / BACK:
  pause stays local) and both feed the peer's pad to controller `CRASH_MP_REMOTE_USER`. So each machine runs
  both Crashes on the same inputs and the game itself plays the same moves on both (run, jump, attacks,
  their effects); no fight-tree state has to be sent. The client's own Crash runs on its own pad at once
  (local prediction: no round trip before it moves). Measured on one PC: a pad change reaches the peer in
  1-3 ms. The client's player 1 / player 2 records swap (the host's P2 is the client's P1, whose camera
  follows it; the host's P1 is the client's P2).
- **Corrections go through the physics**: writing the transform is not enough, a Crash's physics keeps its own
  position and puts it back every frame (measured: 3-4 units behind the host while running), and an animated
  player must not get its transform overwritten every frame anyway. When a player is more than
  `CRASH_MP_SNAP` (1.0) off the host's record, the client calls `CPhysicsBehaviour::Teleport`
  (`sub_82142338`, vector in r4 / r5; the physics behaviour is Crash +0xDC, the Crash = `0x8249D778[index]`,
  its +96 is the actor). A record of a player that is not on foot (record flag bit 1: joining / riding, the
  position is a parking spot) and a local player that is not on foot are never snapped. With mirrored inputs
  the two machines stay within ~0.3-0.5 of each other while running and jumping: no snap at all in the test
  after the start.
- Seen (`mp_m2_host` + `mp_m2_client`, L1_E1): both windows show both Crashes from the first frame; the
  client walks, jumps and attacks with its Crash, the host's P1 walks, attacks and jumps, each window shows
  both in the same places with the same moves (the attack swipe and its hit effect on both). Each machine
  draws its own player with P1's look (orange) and the other with P2's (`Crash_Player2`): the per-player
  colours are M4. What inputs cannot mirror (enemies' own AI, hits that land differently on each machine)
  is M3.
- Tried and dropped: a non-player Crash puppet through the `Cheerleader` class (what the cinematics use for
  Cortex / Tiny / N. Gin): its bank only has the Cortex / NGin / Tiny subtypes, so subtype `Crash` crashes in
  `sub_82389720` (`mp_marionete`). A real puppet needs a new actor template (M3).
- Limits of this route: 2 players per host (the game's), the client's input travels a round trip before its
  Crash moves (fine on LAN, ~2x ping online; prediction = running the client's P1 locally and correcting,
  M3), and remote players beyond P2 still need puppets.

## Recommended architecture

- **One real player per machine.** Each machine runs its own Crash as player 1 (input, physics, camera,
  HUD), so movement is responsive and no code path ever sees more than the game's two players.
- **Remote players are puppets**: a Crash-looking actor that is not a player (no index, no input, no HUD),
  driven by the owner's transform + fight-tree state, recoloured per player. On the host the puppets are
  what enemies target (`CAIManager::AddTarget` exists), what trigger volumes and pickups see
  (`ETriggeredBy_MID_POINT_OF_ALL_ACTIVE_PLAYERS` already exists for 2), and what damage is applied to
  (forwarded to the owner).
- **Host-authoritative world**: enemies, objectives, cinematics, save. Clients suppress AI / objectives and
  mirror host actors by name hash; host events (spawn, death, objective step, cinematic start, level change)
  are messages.
- Streaming: host union, clients their own (above).

## Milestones (rough effort, one developer with these tools)

| Milestone | Work | Effort |
|---|---|---|
| M2 client input drives a Crash on the host | **first slice done** (the host's native P2 on the client's pad, both players on foot from the start, physics teleports, above); input mirroring for animation and local prediction, above); left: puppet template for players 3-5 as AI targets and trigger actors, snap against time-aligned records over real latency | 1-2 weeks left |
| M3 combat / enemies / level change, up to 5 | fight-tree node sync (animation), spawn/despawn and death events, damage forwarding, jacking titans (titan = host actor ridden by a puppet), objectives/cinematics/level change as host events, palettes for 5 colours, death/respawn per player | 5-8 weeks |
| M4 lobby, colour select, internet | lobby menu + colour select (Scrooby screens or our own overlay), protocol versioning / reliability layer, STUN hole punching + TURN relay on a small VPS, reconnect | 3-4 weeks (+ VPS) |
