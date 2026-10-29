# Online multiplayer (prototype, M0 / M1 / M2 / streaming)

Feasibility work for an online mode of up to 5 players (branch `online-coop`, 2026-10-28). Local co-op stays
the game's own 2 players. Full write-up in Portuguese: `runtime/NOTES.md`, "Multijogador online
(protótipo)". Code: `runtime/online/` (`mp.h`). **Everything is off unless `CRASH_MP=1` or a session is
opened from the main menu's MULTIPLAYER** ("Lobby" below); with it off no hook changes anything.

| Switch | What |
|---|---|
| `CRASH_MP=1` | turns the prototype on |
| `CRASH_MP_STREAM=vanilla\|union\|all` | resident rooms (default vanilla = the game's) |
| `CRASH_MP_ANCHOR=x,y,z;...` | positions that count as players in the room choice (stand-ins for remote players) |
| `CRASH_MP_TRACE=1\|<ms>` | rooms + players once a second, every player move > 10 units in one frame |
| `CRASH_MP_ROLE=host\|client` | the M1 UDP mirror; `CRASH_MP_PEER` (127.0.0.1), `CRASH_MP_PORT` (47810; client = +1), `CRASH_MP_HZ` (30) |
| `CRASH_MP_SUPPRESS=ai,objectives` | client: skip CAIManager / CObjectiveManager updates once its level is up |
| `CRASH_MP_PLAYERS=<n>` | 2-8 players (the game's own limit is 4, 5-8 are experimental: "8 players" below); with more than 2, players 3-8 are spawned on every machine |
| `CRASH_MP_SLOT=<n>` | client: its player, 1-7 (default 1; UDP port + slot); the host is 0 |
| `CRASH_MP_SNAP=<units>` | client: a player further than this from the host's record is put back through its physics (default 1.0) |
| `CRASH_MP_NAME=<name>` | the player's name in the lobby (default: the OS user's, A-Z 0-9, 12 letters) |
| `CRASH_MP_PASSWORD` / `CRASH_MP_CODE` | tests: the host's password and what a client's password field starts with / what ENTER CODE starts with |
| `CRASH_MP_TELEPORT=x,y,z` | host, tests: once both players are on foot, put P1 there and P2 2 units further along x (`mp_m3_host`: the first fight of L1_E1) |

Profiles (`test = false`): `mp_stream_vanilla`, `mp_stream_union`, `mp_stream_all`, `mp_host` + `mp_client`
(run together; windows side by side through `CRASH_RESTART_WINDOW`), `mp_tres_crash`, `mp_m2_host` + `mp_m2_client`,
`mp_m3_host` + `mp_m3_client` (both players in the first fight, client with its own AI and objectives),
`mp4_host` + `mp4_c1` + `mp4_c2` + `mp4_c3` (4 machines, windows in the screen's quadrants), `mp_tres_crash` (4 players
on one machine, controllers 0-3), `mp_oito_crash` (8 players on one machine, P5-P8 on our controllers 4-7), `mp_marionete`, `mp_lobby_host` + `mp_lobby_c1` + `mp_lobby_c2` (the lobby through
the menus: host, a client from the LAN list, one by code), `mp_lobby_host_new` (the host's NEW GAME to the HUB).

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

**A MULTIPLAYER session uses union on every machine** (`mp::Stream`, unless `CRASH_MP_STREAM` says otherwise).
The game asks for the resident set every frame while idle (`sub_822502A8`: the room check, then
`sub_82250200(current)` and a load when the load list is not empty), so union follows the players as they
move. With the game's set, a PC + 2 Android session broke: the third player stood in the room where it had
been parked and kept it current, the room the host walked into never loaded on a client, whose player fell
there (no floor) and was knocked out again every 2 s next to the host; the map looked wrong unless everybody
stood together.

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
- `CActor::GetBehaviour(actor, type)` `sub_82142608`, type = the class's vf05: 38 render. It does not give
  the players' fight tree (nor the enemies'): use the Crash's own fields (+0xDC physics, +0xE0 render).
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
- The host's `ReadController` for user 1 returns that pad (connected) while packets arrive (2 s timeout =
  unplugged). Late / duplicated sequences are dropped. (Up to 4 players: "4 players" below.)
- In the game P2 joins with **B / Circle** (into P1's backpack) and the **same button** leaves it; Y
  (Triangle) does not.
- **Both machines open the level with the two Crashes on foot**, the way the game does between two co-op
  levels: `Crash::Crash` copies the co-op state the last level left (`0x8245FA9C[index]`: 0 = not joined,
  1 = in the backpack, 2 = on foot; `Crash::vf20` saves it) into Crash +300, and `0x8245FA98` is the player
  that auto-joins into the backpack at the start (`OtherPlayerAutoJoinAtStart` `sub_82163A40` ->
  join `sub_82154110(player, controller)`: flags +0x110 |= 0x18, objective event 0x2F). The first version
  used auto-join + B taps (P2 in the backpack, then off ~20 ms later). Now P2 comes up like players 3 and 4
  ("4 players" below): state 2 + nobody auto-joining, then shown next to P1. State 2 alone leaves P2's model
  hidden (it sits on P1); state 0 (not joined) frees its input map (`Crash+176` then points at freed memory,
  and P2 ignores its controller), so it has to be state 2 + the show.
- Online nobody is pulled into the backpack afterwards: co-op puts the player the shared camera leaves behind
  into the other one's; each machine has its own camera. The Crash's script asks `IsOkayToEnterBackpack`
  (`sub_82153400`), then `SetPlayerEnteringBackpack` turns the Crash's collision off and
  `EnterOtherPlayersBackpack` (`sub_82154D10`) attaches it. Online the question says no for a Crash on foot
  (state 2), and an entry that still comes takes the game's own failure path (flags +0x110 bit 2: collision
  back on, `DidEnterBackpackFail`); the start join (state 0) still goes through. Skipping the entry alone
  (the first version) left the Crash without collision on the other's back: it fell through the floor and
  was knocked out (co-op state 3), and the fight respawned its enemies under new names.
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
  `CRASH_MP_SNAP` (1.0) off the host's record (at most every 250 ms per player), the client sends a
  `CActionTeleport` (`sub_820972D0(msg, Vector*, flags)`, message type 40) to the player's physics handler
  only (`sub_82145358(physics, actor, msg)`; the physics behaviour is Crash +0xDC, the Crash =
  `0x8249D778[index]`, its +96 is the actor). The scripts' `CPhysicsBehaviour::Teleport` (`sub_82142338`)
  sends it to the whole actor and the Crash then takes the other player along. A record of a player that is not on foot (record flag bit 1: joining / riding, the
  position is a parking spot) and a local player that is not on foot are never snapped. With mirrored inputs
  the two machines stay within ~0.3-0.5 of each other while running and jumping: no snap at all in the test
  after the start.
- Seen (`mp_m2_host` + `mp_m2_client`, L1_E1): both windows show both Crashes from the first frame; the
  client walks, jumps and attacks with its Crash, the host's P1 walks, attacks and jumps, each window shows
  both in the same places with the same moves (the attack swipe and its hit effect on both). What inputs
  cannot mirror (enemies' own AI, hits that land differently on each machine) is M3.
- **Enemies (M3, first slice).** With its own AI and objectives on (`CRASH_MP_SUPPRESS=none`) the client
  spawns the same enemies under the same names, and they walk the same way (same inputs, same AI):
  host and client within 0-1.4 units with only the transform records (`[mp] enemies` trace: the actors
  spawned after the level came up, name, controller vtable, position, hitpoints). What diverged was the
  hits: each machine's Crashes landed different hits, so the host killed an enemy the client kept.
  **Hitpoints are the host's**: every hitpoint change goes through `CDamageableBehaviour::AdjustHitpoints`
  (`sub_82110448`, dev/dev_cheats.cpp's hook calls `mp::Hitpoints`), which also sets every character's
  hitpoints when it comes up, so it learns each actor's damageable (+28 actor, +64 -> hitpoints). The
  record carries them (the old fight-state-time field; -1 = none). On the client the local damage to an
  actor the host mirrors is dropped, and a record whose hitpoints differ is applied through the same
  function; the game's own death (drops included) follows. The players' hitpoints stay each machine's.
- The client rebuilds its name -> actor map from the live room lists every frame: an actor the game
  removed (enemy killed, pickup taken) kept getting the host's last transform written into its freed
  memory, and the client crashed seconds later in `sub_8228D810` (write to 0xC).
- Seen (`mp_m3_host` + `mp_m3_client`, both Crashes in the first fight of L1_E1, the host's P1 and the
  client attacking): both machines with the same enemies all along (3 -> 10 with the drops -> 9 -> 2), the
  same ones dying at the same time, positions within 0-1.0; the client took the host's hitpoints and
  dropped its own hits; no crash in 60 s. Left: hits the client lands with nobody on the host to see them
  (the host plays the client's Crash on the same pad, so they land there too unless positions diverge),
  enemies the host's camera never sees. (Knock-out / respawn: "4 players" below.)
- **Same colour on both machines.** Both Crashes' drawables (`CCharacterObject`: Crash +0xE0 is the
  `CCharacterRenderBehaviour`, vtable 0x82015BC4, its +92 the drawable) hold the same palettes: normal
  (`crash_normal`) and jacked (`Crash_Player2`) of their variant (+4548; +4516 / +4532). At setup the normal
  one is copied into the drawable's base (+348, `sub_8222C2D8`); every frame `sub_822212C0` blends the base
  toward the jacked one by +0x15D0, which stays at the jacked end for player 2. The client's P1 is the host's
  P2, so the client swaps them once per drawable when both are on foot: its P1 gets the jacked palette as
  base, its P2's jacked slot points to the normal palette. Seen: the host's P1 orange and the client's player
  pale on both windows from the first frame. The head marker stays per machine ("1" over the local player).
  Up to 5 colours = more palettes (recoloured at load, as `settings/snipe_colors.cpp` does) chosen the same way.
- Tried and dropped: a non-player Crash puppet through the `Cheerleader` class (what the cinematics use for
  Cortex / Tiny / N. Gin): its bank only has the Cortex / NGin / Tiny subtypes, so subtype `Crash` crashes in
  `sub_82389720` (`mp_marionete`). A real puppet needs a new actor template (M3).
- Limits of the first route: 2 players; the third and fourth are native Crashes too ("4 players" below).

## 4 players

The game's own limit is 4: 4 controllers (`CInputManager`, manager 7, has 4 `CInputDevice`s, vtable
0x8201DC54, controller number at +0x10) and 4 AI targets (`CAIManager` holds 4 36-byte slots from +4 with the
count at +0x94; a 5th add writes over the count and the AI update `sub_820985E0` then runs over its stack
arrays). So up to 4 players, all native Crashes, no puppets:

- **Spawn**: with `CRASH_MP_PLAYERS` > 2 every machine runs one objective of the game's own system,
  `DO_SpawnPlayer(3, "ref_Crash_P3", "PlayerSpawnPosition")` (and 4), once per level when P1 is on foot
  (`dev::RunLua`). The game gives every Crash past the second index 2; the constructor hook gives 3 and 4
  (`Crash+292`), puts back the words its table store hit past the 2 entries (`0x8249D780..788`: backpack Crash,
  rider, last rider) and sets co-op state 0. A 5th Crash is left hidden.
- **Shown on foot**: P2-P4 start hidden and parked like an unjoined P2 (`sub_82154900` turns their behaviours,
  collision and AI target off). Once P1 is on foot each is shown with the game's own "leave the backpack"
  show, `sub_82154A58` (behaviours, collision, AI target on; it detaches the Crash from the backpack Crash's
  attacher, so `0x8249D780` is P1 for the call), set to co-op state 2 and teleported next to P1.
- **Controller**: `Crash+176` -> an input binding whose +0 is the `CInputDevice`; P3 and P4 start on P2's, so
  they get devices 2 and 3.
- **No backpack**: online (or with `CRASH_MP_PLAYERS`) `SetPlayerEnteringBackpack` (`sub_82153348`) is skipped for a
  Crash on foot, `IsOkayToEnterBackpack` says no and `EnterOtherPlayersBackpack` fails the game's way.
- **Network**: slots (host 0, clients `CRASH_MP_SLOT` 1-3, UDP port + slot). Each machine plays its own slot as
  P1 and the others on P2-P4 in slot order (`LocalIndex` / `SlotOf`); the players have the same names
  everywhere, so a host record of player index g is slot g. "CMI2" pad packets carry the slot; the host
  learns each client's address from them and passes them on to the other clients; each machine gives
  slot g's pad to controller `LocalIndex(g)`.
- **4 colours, by slot**: orange (slot 1, normal palette), pale (slot 2, jacked), blue (3), green (4), fixed
  (base and blend target the same palette, so the jacked blend never moves it). Blue and green: after the
  game's palette blend of the frame (`sub_822212C0`, hooked) the final palette (+0x11C8, 256 big-endian
  ARGB words) of those players' drawables gets its fur turned: the saturated oranges (hue 5-45 degrees,
  saturation > 0.5; skin, jeans and shoes are outside it) go to hue 215 / 115, saturation and value kept.
- **Knock-out and back**: a player's death posts a `CActionResetPlayer`; its first run (`sub_8220D830`, the
  only caller of the co-op knock-out `sub_82164F40` -> `sub_82164120` -> `sub_82154300`) puts a co-op
  player into the other's backpack in co-op state 3 with a timer at +0x114 (`Crash::vf08` counts it down,
  `sub_82152D28`, then state 0 = not joined), and has no Crash for an index >= 2 (P3/P4 got no action at
  all and lay there for good). Online (or with `CRASH_MP_PLAYERS`) every player is knocked out the same way
  instead, on that action or on its hitpoints reaching 0: it lies for the game's knock-out time (float at
  0x82002328, 5 s), then its actor is reset (`sub_82090370`, actor +0x1C vtable +0x3C, what the game's
  respawn calls: up again, full hitpoints; two values kept as `sub_8220D830` does) and teleported behind the
  on-foot teammate with the lowest slot, 1.2 apart per slot (the same place on every machine).
- **Players' hitpoints are the host's**: player records carry hitpoints as a fraction of the maximum (P1/P2
  start at the game's 30, P3/P4 at the template's 90: they get P1's maximum; machines may differ with the
  upgrades); the client drops its own hits on players and applies the host's fraction through
  `AdjustHitpoints`, so a knock-out happens on every machine, except for a player already knocked out or
  back for less than 1.5 s (the host's record may still say 0). A player's damageable is found before its
  first hit among the pointers of the actor's behaviour controller (actor +0x1C; vtable 0x8201145C, its
  actor at +28): it is not always inside the actor.
- **Camera-relative sticks**: the game walks a player along `stick.x * right + stick.y * forward` of the view
  camera (`input::StickToWorld` `sub_821EBC70`, right = (forward.z, -forward.x), `sub_82253818`; forward =
  CRenderManager +356 VectorCamera +228). A remote stick only means the same direction under the same
  camera, so each pad packet carries the sender's camera forward and the receiver turns the left stick onto
  its own camera's axes.
- **Each machine's own camera**: the gameplay camera frames every actor with a registered
  `CCameraInterestBehaviour` (vtable 0x8200DB6C; `vf11` = SetEnabled -> `sub_820E10E8` adds it to the
  `CCameraManager`, manager 4, `sub_821B2830`, and removes it, `sub_821B2950`; +0x3C bit 0x40 = registered,
  +0x38 its id). Every Crash has one, so all 4 machines showed the same view framing the whole group (the
  shared co-op camera). Online the remote players' interests are taken out every frame (a show turns them
  back on): each window follows its own player. Local co-op (`mp_tres_crash`) keeps the shared camera.
- **Fewer corrections**: player 1 is in play 500 ms after the level shows it and P2-P4 300 ms after being
  shown (their transform holds the parking place, 57.4 45.5 -52.1, until then; a client snapped its P2 there
  and back); no correction for a knocked-out player or one just back; moving within 1.6 of another player
  (pushing: each machine sees the push a few frames apart) up to 2.0 is let go (the standing correction
  evens it out); a correction to the same place that does not hold is tried 3 times, then left until the
  host's record moves (`mp_m3_client`: 20 snaps in 60 s, 15 of them to one spot, now 6).
- Seen (`mp4_host` + 3 clients, L1_E1): the 4 Crashes on foot on every machine, each moving its own and every
  machine showing each slot in the same place (within 0.5-0.9), same colour per slot on every machine; 6-17
  snaps per machine in 40 s (most at the start), errors after the spawn up to 1.7 and brief. Knock-out
  (`--set dev_invincible=false --set dev_lua_file=tools/dev_lua/mp_ko.lua` on `mp4_host`, which kills P3 then
  P2; `mp_ko` on one machine): the same two players down on all 4 machines, back 5 s later at the same
  place (within 0.2) with 30 of 30. A player that stands still (the host's record and the local
  Crash both still for 0.5 s) is corrected past 0.25 instead of `CRASH_MP_SNAP`: one client kept its own
  player 0.9 off the host's for good; now standing players match within 0-0.22. The first try showed a remote P2 in T-pose, stuttering: it
  ignored its controller (state 0 had freed its input map), so the snaps pulled it every 250 ms.

## 8 players (experimental)

Past the game's 4, up to 8 native Crashes (branch `mp-8players`, 2026-10-29). The lobby's MAX PLAYERS goes to 8
(default 4); 5-8 show EXPERIMENTAL. `mp::MAX_PLAYERS = 8` / `GAME_PLAYERS = 4` (`mp.h`), `lobby::MAX_PLAYERS = 8` /
`STABLE_PLAYERS = 4` (`mp_lobby.h`, protocol version 5); every per-player array of `mp_net.cpp` / `mp_players.cpp` is
sized by them (pads, positions, snaps, hitpoints, camera interests, knock-outs), slots 0-7, UDP port + slot.

- **Spawn**: `DO_SpawnPlayer(3..8, "ref_Crash_P3..P8", "PlayerSpawnPosition")`, the same objective as players 3-4.
  Our index (the `Extra` list: `PlayerCrash`, knock-out, colours, corrections) is 2-7, but the byte the game
  reads (Crash +292) is `GameIndex`: 2 or 3 (players 5 / 7 share player 3's, 6 / 8 player 4's). The game's
  per-player byte flags (CGameDatabase +32 + index: set by `Crash::vf06` `sub_821648A0`, cleared by `SignalDead`
  `sub_82153E70`, read by `Crash::vf15` and others) have room for index 0-3; index 4-7 would write into the int at
  +36 after them. Its other uses of the index are guarded (`index > 1` checks, compares with 0 / 1 / 2) and
  `sub_821D8C50` (player -> controller, a table of 2) is already read past for 2 / 3. A `CActionResetPlayer` of
  index 2 / 3 goes to whichever of the sharers is out of hitpoints (else the game's 2 / 3).
- **Controllers 4-7** (`ExtraDevice`, `UpdateExtraInputs`): the game polls 4 XInput users (controller system
  `*0x824B46DC`: 4 controllers of 48 bytes from +24; +16 user, +28 inputs of 60 bytes, +32 outputs of 36, +36 type,
  +40 connected; per type at 0x824733B4 + 28 n: +12 inputs, +20 outputs) and `CInputManager` builds 4
  `CInputDevice`s (524 bytes, `sub_821ECDD0(this, 1, 40, n)`, vf0 = AddRef). A controller input
  (`sub_8228A110(input, XINPUT_GAMEPAD*)`) calls back the device attached to it (+52 the callback = device +20,
  +56 the device's input) when its value changes; the device (`sub_821ED228` -> `sub_821ECC78`) keeps a history
  per input (+48 + 12 i: buffer, size 40, position) and its update (`sub_821ED160`, from `CInputManager::vf10`
  `sub_821ED5B0`) runs the input maps of the Crash it drives. For players 5-8: a copy of controller 0 (its 20
  inputs and outputs copied, callbacks cleared), a device built with the game's constructor and attached by the
  game's own `sub_821EC1C8`, neither in the game's lists. After `CInputManager::vf10` each frame the pad of
  controller 4-7 (`input::ReadExtraController`: `CRASH_INPUT_SCRIPT`'s `P5_..P8_` or `mp::RemotePad`) goes into the
  copy (big-endian at +48, the poll's stick dead zone `sub_82289FF8` with the float at 0x8200DD94), through each
  input, then the device is updated (time step of the game's last device update). The Crash's binding (+176 ->
  +0) points to it, with a reference of ours (the Crash releases it when it goes; ours keeps it alive).
  In a level the game's heaps are full: `operator new(524)` had room for one device and its constructor's
  allocations (18 histories of 644 bytes, the stick detectors at +492..+504) came back null (a null history was
  the first crash, `sub_821ECC78` reading 0x4). So the device is in the runtime's memory (`GuestAlloc`) and what
  its constructor could not allocate is made there the way it does (`sub_821E9C78` per history entry,
  `sub_821EB3F0` per detector). Made once per controller, kept for good.
- **AI targets** (`BalanceAiTargets`, only with more than 4 players): `CAIManager` keeps 4 targets (36-byte slots
  from +4: +0 actor, two lists of the AI after it at +4..+8 and +20..+24, count at +0x94) and its update
  (`sub_820985E0`) works per slot with stack arrays: it cannot grow. Every 0.5 s a target not in play (knocked out,
  gone) gives its place to a waiting player, a free place goes to whoever waited longest, and every 8 s the target
  nobody is after (both lists empty) that has been one longest makes room for the longest waiting: enemies go after
  every player in turn. Players' actor = Crash +156 (what the show adds, `RemoveTarget` takes). Each machine
  decides on its own clock (the enemies' positions and hitpoints are the host's anyway).
- Knock-out / revive, hitpoints (players 3-8 get player 1's maximum), leash, `CorrectRemotePlayers`, `Gather`
  after a cutscene, `RemovePlayer` and the camera interests loop over the 8.
- **Colours**: slots 5-8 come from `mp-colors` (its own branch); here they are orange like slot 1.
- Seen: `mp_oito_crash` (one machine): the 8 Crashes on foot next to P1, P5-P8 each walking its own way on
  controllers 4-7, no crash in 30 s, AI targets turning (player 5 in, then 6, 7...). Five machines (`mp4_host` +
  `mp4_c1..c3` + a fourth client, `--set env={..., CRASH_MP_PLAYERS = "5", CRASH_MP_SLOT = "4" ...}`, windows 3 + 2):
  every machine with the 5 Crashes, slot 5 walking on its machine and in the same place on the host and client 1
  (-54.7, -2.5, 53.6 on all three), 16-21 snaps per machine in 40 s (as with 4).
- **Not done / limits**: a CActionResetPlayer with no sharer out of hitpoints (a fall) knocks out the game's
  player 3 / 4 instead of 5-8 (the leash brings a fallen player back); enemies chase at most 4 players at a time;
  the lobby's player list with 8 names does not fit the 720 virtual height (`mp-menus` lays it out); never tried with 6-8
  machines, on Android nor over the internet; each extra controller takes ~13 KB of the runtime's heap.

## Lobby (MULTIPLAYER on the main menu)

- **The item**: a sixth main menu item, MULTIPLAYER, between LOAD GAME and CREDITS (`settings/pc_frontend.cpp`:
  a copy of the CREDITS item in `GameStart_Start_Xenon.pag` / `StartMenu`, the items above it half a step up and
  the ones below half a step down; its text a new key, `frontend` / `FE_Button_Multiplayer_Text`, added to the
  bibles of both frontend packages as "Definitive Edition" is, `title_logo.h` `AddFrontendTextKey`). The
  game's conditions know items 0-3 (GetMenuIndex `sub_820F9A10` compared in `sub_820F9BA8`), so the hook maps
  the indexes back (3-5 -> 2-4; EXIT GAME stays 4) and MULTIPLAYER to SETTINGS (3): it takes exit 91 to the
  emptied PS2 options screen (node 99) and its marker (`PlaySoundSelect(4242)`) opens the MULTIPLAYER screens
  (`online/mp_menu.cpp`) instead of the settings, drawn the same way (game fonts and frames, overlay channel
  `BITMAP_MULTIPLAYER`, the game's input neutral while open).
- **Screens**: HOST GAME (NEW GAME / LOAD GAME) and FIND GAME (SERVER LIST / ENTER CODE, then a password if the
  host has one). The host's choice opens the session and closes the screen without B: the next condition of
  node 99 returns the main menu's own exit, 88 (NEW GAME -> name entry) or 89 (LOAD GAME -> the slot screen); a
  new game's last screen (node 322, `FadeOut`) takes exit 309, the loaded game's `ExitToHUB` (`EndAccess` +
  `FadeIn` -> 575, the HUB), instead of 323 (`StartLevelByName(L1_E1)`); exit 380 (`GotoGameState(LEVEL_SELECT)`)
  got there too but stayed black (no fade in). A loaded game reaches the HUB by itself (308 -> 309 -> 575). A
  client waits in LOBBY (the host, the players, their ping); B leaves.
- **At the HUB** (node 575) the host has a panel at the top right: players, the invite code; BACK (hidden from
  the game) opens the lobby: A on a player kicks that player, PASSWORD (typed), MAX PLAYERS 2-8 (5-8 marked EXPERIMENTAL), KNOCK-OUT TIME 3 / 5 / 10 s
  (the knock-out of "4 players", instead of the game's float), CLOSE SESSION. Y (CONTINUE STORY) or any level
  start of the HUB goes on as usual: when the host reaches InGame (node 630) its level index (managers +24) goes
  to the clients in the roster; each one, on its options screen / main menu / HUB, calls `Frontend:StartLevel`
  (`sub_820F5D58`) with it and takes SkipToLevel (exit 51), as the console's `level` does. Joining is refused
  while the host is in a level; the roster's player count is fixed then.
- **Protocol** (`online/mp_lobby.h`): "CML1" UDP packets on the game's socket, which now has an I/O thread
  (`mp_net.cpp`): lobby packets are handled there, the mirror's are queued for the game thread. DISCOVER (a
  client, every second, to 255.255.255.255 and 127.0.0.1) / ANNOUNCE (name, players, maximum, password, in a
  level), HELLO (token, name, password, every 0.5 s until) WELCOME (slot) or DENY (full, password, kicked, in a
  level, version, host left), ROSTER (every 0.5 s: players, options, the level start counter), PING / PONG, LEAVE;
  6 s of silence drops a player (or the host). Slots stay 0..n-1 in join order while no level runs; the session
  sets `mp::Players()` and the mirror's role / slot / addresses (`mp::net::SetRole`, `SetClientAddress`), so a
  level started from the lobby is the `CRASH_MP_ROLE` prototype with the lobby's players. Host port 47810
  (`CRASH_MP_PORT`), clients any. The invite code is the host's address in 7 Crockford base-32 letters
  (`XXXX-XXX`; 10, `XXXXX-XXXXX`, with a port other than 47810): its public one once "Internet" below found
  it, its LAN one otherwise. HELLO carries the password hashed (FNV-1a 64, protocol version 2).
- Text entry: letters and digits typed on the keyboard (`input::pc::SetTextEntry`), keys on the screen (mouse /
  touch), or the D-pad (up / down the letter, left / right the cursor, X erases).
- Mouse and touch (Android's menus are touch as a mouse): a click on a row chooses it, on the game's B BACK /
  SELECT A hints is that button, on an option's left / right half changes it, on the HUB panel opens the lobby;
  kicking needs a second tap on the player. On Android the default name is the device's model.
- PC + Android: the same protocol (one UDP socket, `net::Broadcast` with SO_BROADCAST); a Windows host needs the
  firewall to let the game take UDP 47810 (the prompt on the first HOST GAME, private networks).
- Seen (`mp_lobby_host` + `mp_lobby_c1` + `mp_lobby_c2`, three windows): MULTIPLAYER between LOAD GAME and CREDITS
  (the six items centred), HOST GAME > LOAD GAME > slot 1 to the HUB with the panel (code, 3/4 players); the
  list client found the host (LAN address and 127.0.0.1 answers merged) and was asked the password, the code
  client was refused (WRONG PASSWORD), typed it and joined; the lobby at the HUB (players with ping, password,
  options); Y on the host: all three in the host's level, 3 players, each window its own Crash (orange, pale,
  blue). `mp_lobby_host_new`: HOST GAME > NEW GAME (name, NORMAL, slot 1) straight to a new save's HUB with the
  panel and the lobby. Left: a client plays without the host's save (its own progress: mojo 0, upgrades), follows a start only
  from its menus / HUB (not from inside another level); the host's NEW GAME leaves the opening level to CONTINUE
  STORY at the HUB.

## Internet (no server of ours)

- **The host's port** (`online/mp_nat.cpp`, on its own thread when the session opens; `CRASH_MP_NAT=0` skips it,
  as `run_game.py` does for the LAN tests): STUN (`stun.l.google.com`, then `stun.cloudflare.com`) on the game's
  own socket gives the public address (the answer comes through the I/O thread, `nat::OnPacket`); UPnP IGD maps
  UDP 47810 on the router: SSDP M-SEARCH sent from the LAN address (`IP_MULTICAST_IF`: Windows otherwise picks a
  virtual adapter), the description and SOAP with `Host:` the router's address (MiniUPnPd answers 404 to a
  name), `AddPortMapping` (lease 2 h, 0 on error 725, the next port on 718).
- **Two routers** (an operator's modem and a router of one's own, common here): the inner MiniUPnPd answers an
  empty `GetExternalIPAddress` (its WAN address is private); the modem above is found with an ICMP echo of TTL
  2 towards the STUN server (Time Exceeded from it) and the inner router's WAN address with an echo to the modem
  carrying the IP record-route option (`IcmpSendEcho`, no admin needed; Windows only); SSDP unicast to the modem,
  then its `AddPortMapping` to that WAN address. Open = mapped and the router's public address equals STUN's.
- **States** on the HUB panel and the lobby: checking / open / "forward UDP port 47810 on the router" (STUN
  gave a public address, no UPnP) / "your network does not allow it (use a VPN)" (no public address: CGNAT).
  Closing the session deletes the mappings (both routers) on a thread; a stale Open never deletes a newer one.
- **Hardening**: the host takes pads only from the address the lobby gave each slot; a client only from the
  host; more than 600 packets a second from one address are dropped for that second.
- Seen (`mp_nat_host`, TP-Link MiniUPnPd 2.2.2 behind the operator's MiniUPnPd 2.2.0): "routers 192.168.0.1 +
  192.168.15.1 via 192.168.15.3, external = STUN's: open to the internet", both mappings removed at close. Not
  yet joined from outside the network.

## Leaving a level, the pause, players who leave

- **Each machine owns its player's position** (`mp_net.cpp`, protocol version 4): the pad packet also carries
  where the sender's player is and whether it is in play (48 bytes); every machine corrects its copies of the
  other players to that (`CorrectRemotePlayers`, the rules the client's corrections from the host's records
  had: past 1.0 moving, 2.0 in contact, 0.25 standing, 250 ms, 3 tries) and never its own player. Before, a
  client's own Crash was corrected to the host's copy of it: a teleport every now and then while walking, with
  the falling animation. The host's records still give the players' hitpoints.
- **Leash** (as the shared co-op camera kept the players together, each with its own camera;
  `CRASH_MP_LEASH`, default 15 units, 0 = off): past it from the middle of the other players in play, the part
  of player 1's stick that walks further away is taken out before the game and the other machines see the
  pad (`Leash`; the game thread keeps the way away for the input reads, `UpdateLocalPlayer`). A player still
  further than twice that for 1.5 s (a fall, a hit) goes back next to the teammate with the lowest slot, moved
  by its own machine (not during a cutscene or a video).
- **No pause in a session** (`mp_menu.cpp`): `Frontend:PauseGame(true)` (`sub_820F8918`, CUberManager vf13) is
  skipped while the frontend is on GameRunning (node 638): the host's pause screen, the tutorials and the other
  screens it opens show over the running level, and the other machines go on with it. The host gets the
  one-player pause (RESUME GAME, OPTIONS, SAVE AND EXIT; no DROP OUT): in a session
  `CFrontendManager::HaveBothPlayersJoinedGame` (`sub_821DAAB0`) says no to the frontend (the pause's page,
  COND TwoPlayer / OnePlayer in `sub_820F6420`, chosen once when the pause opens, and its exits in
  `sub_820F9BA8`), but not to `UpdateInputAndCheats` (`sub_821DB680`), where "not both joined" lets another
  controller drop in as player 2.
- **A client's START** (exit 651, ExitPause, from node 638) is refused and opens the client's own menu over the
  game (`Screen::LevelMenu`, the game's pad neutral while open): RESUME, SETTINGS (the settings screen over
  the running game, `settings::menu::RequestOpenOverGame`: no B for the game when it closes) and QUIT TO THE
  MAIN MENU; B / START resume. QUIT leaves the level the no-save way (1114 -> 1115 -> 1116 -> the HUB, the save screens' exits replaced
  by 1179) and goes on from the HUB to the main menu (598), leaving the session: only the host keeps a save.
- **The host's EXIT LEVEL** (pause; DROP OUT is EXIT LEVEL online, 1098 -> 1099) saves and goes to its HUB; the
  roster's in-level flag goes off and every client leaves the level the same no-save way, then from its HUB to
  the main menu, where the lobby opens again at the wait (exit 91 + the SETTINGS marker): the host's next level
  is followed from there. The host back at the main menu after its HUB closes the session (DENY host left):
  the clients' lobby says THE HOST LEFT; a client still in a level goes to the main menu.
- **A player who leaves** (gone from the roster: LEAVE, or 6 s of silence) is taken out of the level on every
  machine (`mp::RemovePlayer`): the behaviours `sub_82154A58` turned on when it was shown (Crash +224, +180,
  +220, +208, +216, +212, +244, +240, +228, +236) get vf11(0), the physics' collision off (`sub_82143698`),
  +272 bit 0x10 off, its AI target removed (`CAIManager::RemoveTarget` -> `sub_82098E68`); it is out of play
  for the knock-outs, snaps and colours.
- **Unloading a level with 3-4 players** crashed three ways, all fixed: (1) the P3/P4 spawn objective, made by
  `dev::RunLua` outside the level's script, was resettable: the level's end reset it, its `CActionSpawnPlayer`
  (`sub_822116C0` -> `CActionSpawnActor` `sub_822102B0`) ran again with the template's name already freed
  (`GenerateObject` gsub on nil, then a null call): now `NewObjective(name, "", true)` (not resettable);
  (2) players 1 and 2 going before an extra Crash released the players' shared object (0x8249D78C) the extra
  then used (`CActor::PrepareForDelete`): the ~Crash hook keeps the extra in the other table entry meanwhile;
  (3) online, a replaced jacked palette pointer made the drawable release its normal palette twice
  (`_purecall` in `~CCharacterObject`, reached from `CActor::PrepareForDelete` before ~Crash): put back in a
  `~CCharacterObject` hook (`sub_822210E0`). KeBugCheck now logs the host backtrace (guest callers by name).
- **Off the camera** a remote player was sent to the backpack every frame (no animation, no hits): the fight
  tree's "GoStraightToBackpack" (`sub_820A0160`) asks `sub_821551B0`, true while the Crash's cached "okay to
  enter the backpack" (+272 bit 0x80, `sub_82154E78`) is on, and only SetPlayerEnteringBackpack (skipped online)
  cleared it: the hook clears it for the Crashes on foot.
- **Cutscenes**: only the host skips one (`settings/skip_cinematics.cpp`: a client gets no prompt and the game
  doesn't see its skip buttons). The host's skip goes in the roster (a counter and the cutscene's frontend
  node, protocol version 3); a client skips that cutscene when it plays it (kept 20 s for a client still
  behind). When a cutscene ends (state 7 on a cutscene node; the scripted sequences inside the gameplay don't
  count) every player in play goes next to the host's 500 ms later (`mp::CinematicDone`, 1.2 apart per slot):
  the game places players 1 and 2, and players 3-4, spawned during the level's opening cutscene, stayed at the
  parking place (57.4 45.5 -52.1), out of everybody's sight: "the blue Crash was invisible". Profiles
  `mp_cine_host` + `mp_cine_c1` + `mp_cine_c2` (not passing yet: the clients' scripts end before they join).
- Test: `mp_quit_host` + `mp_quit_c` + `mp_quit_c2` (three windows, the code client quits by its menu, the host
  leaves, then closes the session); `mp_tres_sair` (local, 3 players, EXIT LEVEL to the HUB).

## Recommended architecture

- **One real player per machine, the others native Crashes on its inputs** (up to 4, the game's limit; 8
  experimental):
  each machine runs its own Crash as player 1 (input, physics, camera, HUD) and the remote players as
  players 2-4 on their owners' pads, so the game itself animates them, enemies target them and triggers see
  them; the host's records correct their positions. (The earlier plan, puppet actors driven by transform +
  fight-tree state, is not needed for 4.)
- **Host-authoritative world**: enemies, objectives, cinematics, save. Clients suppress AI / objectives and
  mirror host actors by name hash; host events (spawn, death, objective step, cinematic start, level change)
  are messages.
- Streaming: host union, clients their own (above).

## Milestones (rough effort, one developer with these tools)

| Milestone | Work | Effort |
|---|---|---|
| M2 client input drives a Crash on the host | **first slice done** (the host's native P2 on the client's pad, both players on foot from the start, physics teleports, above); input mirroring for animation and local prediction, above); players 3-4 as native Crashes on their own controllers, 4 machines, above; left: snap against time-aligned records over real latency | 1-2 weeks left |
| M3 combat / enemies / level change, up to 4 | **first slice done** (same enemies on both machines, host hitpoints and deaths, above); fight-tree node sync (animation), spawn/despawn and death events, jacking titans (titan = host actor ridden by a puppet), objectives/cinematics/level change as host events, palettes for 5 colours, death/respawn per player | 5-8 weeks |
| M4 lobby, colour select, internet | **first slice done** (MULTIPLAYER menu, LAN list, code, password, kick, options, "Lobby" above); left: colour select (Scrooby screens or our own overlay), protocol versioning / reliability layer, STUN hole punching + TURN relay on a small VPS, reconnect | 3-4 weeks (+ VPS) |
