# Frontend, direct boot, cutscenes and saves

Full write-ups: `runtime/NOTES.md` "Frontend de PC", "Boot direto, intros puladas e eventos para testes",
"Pular as cenas", "Teclado do nome e apagar saves"; GUIA "Boot direto e eventos", "Pular cenas e vídeos",
"Nome e saves no PC".

## The frontend tree is native code

- The Frontend fight tree's conditions (which exit a menu screen takes) are compiled into the xex:
  `sub_820F9BA8(tree, node, ...)` returns the exit number; `sub_820F9A10` = GetMenuIndex(tree, page, menu).
- `fighttrees\Frontendlua.blua` holds the same logic in Lua but **the game never reads it** (verified with
  `CRASH_LOG_ALL_IO`). Patching it does nothing.
- Change menu behaviour by hooking those functions (`runtime/settings/frontend_hooks.cpp`) plus same-size
  bfig/page patches (`runtime/settings/pc_frontend.cpp`). The `sub_820F9BA8` hook also calls
  `settings::menu::NoteFrontendNode(r4)` at its top; keep that line.
- Nodes and exits share one numbering, in `Frontend.bfig` file order; `python tools/frontend_tree.py` prints
  the whole tree. Useful ones: 44 LicenseScreen (5 s timer, exit 45), 46 LoadPersistentPostBootUp (exit 51
  SkipToLevel if state 5, else 52), 59 title (exit 65), 70 main menu (89 LOAD GAME; Xenon items 88-91, Y 94,
  EXIT GAME uses 86), 154 save slots (180 + 20 * (slot - 1); slot-screen Y 265 / 523), 308 load complete
  (309), 575 HUB (604 continue, 600 to level), 630 InGame, 633 / 655-1051 cinematic nodes, 638 GameRunning,
  1081 pause, 1154 pause > OPTIONS.

## Direct boot (`runtime/settings/direct_boot.*`)

- `--skip-intros` / `CRASH_SKIP_INTROS=1` / `skip_intros = true`; `--boot` / `CRASH_BOOT` / `boot = "..."`
  with `menu`, `level:L1_E3`, `load:N`, `load:N:hub`, `load:N:L1_E3`.
- Frontend Lua methods: `StartLevel(idx)` sub_820F5D58, `StartLevelByName` sub_820F5D98,
  `GetSelectedItemUnlockableIndex` sub_820F62E8. Managers table 0x824A7A24 (manager n at +(n+7)*4): level
  index +24, levels +156 (count +316, name at level+32). 21 levels; index + 1 = episode number (index 20 is the
  hidden `SoundTest`).
- Logo videos: `PlayMovie(idx)` sub_8214ABD0 (end callback sub_8214B210); skipping a video = asking for the
  missing Sierra one (2).
- **Traps**:
  - The title must get a REAL START press (the controller that presses START becomes player 1 via
    sub_821DB680 -> sub_821FC498). Forcing exit 65 leaves no active player and LOAD GAME goes to node 114
    NoProfile.
  - Player 1 needs ~1-2 s in a menu before a level starts. Leaving a menu 3 frames in = no player 1, the first
    START in the level joins as P2, and pause never opens. So HUB uses a real Y, and `level:` waits 60 ticks
    on the main menu, then calls StartLevel + exit 51 (the tree accepts 51 from node 70).
  - Forcing the opening NIS's exit (758) leaves the game on LOADING forever; see "Skipping cutscenes".
- `managers+352 bit 0x80` is HasXenonProfileChanged, not a skip flag.

## Test events

The runtime logs `[test] event <name> (frame F, input clock S s, tick T)`: `title`, `main_menu`,
`save_slots`, `load_complete`, `hub`, `level_start`, `level_ready`, `cinematic`, `skip_prompt`,
`cinematic_skipped`, `video_skipped`, `cinematic_done`, `gameplay_ready` (node 638 for 45 ticks), `pause`,
`game_over`, `boot_done`, `boot_failed`, `name_entry`, `save_deleted`.

In `config/run.toml`: `input` anchors `["@gameplay_ready"]`, `["@event#2"]`, `[t, "SHOT"]`; `replay` entries
`[t, "EVENT", "x"]`; `expect.events` / `expect.events_absent`. **New tests should anchor on events, not on
wall-clock presses** (wall-clock presses desync when the machine is busy). `tools/convert_recording.py`
converts old F5 recordings to the direct boot.

## Skipping cutscenes (`runtime/settings/skip_cinematics.*`, `cutscenes::`)

- A cutscene = `CMasterCinematicAction` (update vf05 sub_821BA100, state at +412, `SetState` sub_821B92E8) in
  state 3 **and** frontend node 633 or 655-1051. L5_E1 runs master cinematic actions during gameplay
  (node 638) every 5-8 s, so the node check is required.
- In state 3 the time (`[action+12]+124`) follows the sound stream, so speeding the clock is useless.
  `SetState(4)` takes the natural end path (4 end, 5 fade, 6 post tracks, 7 done flag) and the frontend leaves
  the NIS node by itself. Videos are skipped with 4 reads of START (the game's own skip).
- Player skip: hold B/START/A (Esc/Space/Enter, or the mouse on the prompt) 0.8 s; `[game] skip_cutscenes`.
  Test/automation skip: `--skip-cinematics` / `CRASH_SKIP_CINEMATICS=1` / `skip_cinematics = true`
  (profile `vila_sem_cena`: L1_E1 village playable at ~14 s instead of ~115 s).
- The "hidden until released" check must use the **raw** buttons (before masking); the first version freed
  them at once and Esc (= START in node 638) opened the pause on release. `CRASH_SKIP_TRACE=1` logs every read.
- Each video/cutscene needs its own prompt id. `subtitles::EndCutscenes()` clears cutscene tracks on skip.
  START does not pause during a NIS. Reuse `cutscenes::FilterGameInput` / `Draw` for new skippable sequences.

## Name entry and save deletion (`runtime/settings/save_screens.*`)

- Name = global `char[32]` at 0x82497328, length at 0x82497348, max 5 chars, only A-Z / 0-9.
  `CNameEntryScreenAction`: wheel index this+44, add sub_820B44C0, erase sub_820B4568, DONE = bit 0x80 at
  soundplayer+16653 -> node 96 exit 97. Wheel sprites Char_a..Char_9 + Divider_* hidden with alpha
  (sub_822EEA18).
- The game has an unused delete: `core::XenonSaveDrive::vf19` sub_82296710(drive, "CrashOTT GameSlot N") ->
  XamContentDelete; drive = `this` of sub_822959D8.
- Deleting does NOT refresh the slot screen (it is read on entry via nodes 111 -> 148 -> 154). Forcing exit 314
  (LoadFailed's ExitToGameSlotScreen) from node 154 re-reads it.
- `pc_input` text-entry mode (`SetTextEntry` / `TakeTypedText`): letters become text, not menu keys.
- Profiles `nome_teclado_*`, `apagar_save*`.

## Game options and volumes

- `[game] coop_mode, invert_axis, autosave` (`settings/game_options.cpp`), enforced every controller read:
  coop byte 0x8245FA99 (1 leapfrog, 0 piggyback); FrontendManager (6) +16652 bit 0x02 = invert;
  SaveGameManager (13) +100 bit 0x10 = autosave, +99 slot index.
- Live volumes (`audio/game_volumes.cpp`) hook sub_82241838 / sub_82241830; holder = *0x824A8FB0, +4 music,
  +8 dialogue, +12 sfx, value at +36.
- Settings restart: `CRASH_RESTART = relaunch | exit | off`; `run_game.py` uses `exit` (exit code 4) and
  relaunches itself.

## Settings rows: one definition for the game and the launcher

- `runtime/settings/options.{h,cpp}`: `options::Item`, `Tabs()` (6 tabs, row order the `configuracoes*` /
  `controles_tela` / `iluminacao_tela` profiles count on), `Key`, `Label`, `Value`, `Help`, `Percent`,
  `Change`, `RestartOnly`, and the launcher's `LightingPreset` (ORIGINAL / REMASTER / CUSTOM). Both
  `settings_menu.cpp` and `launcher/launcher.cpp` read them; add a new option there, not in either screen.
- Screen-specific: `options::Context` (launcher wording, `canRestart`, DLSS/FSR availability, subtitles),
  PLAYER 1/2 value and cycling (the game lists connected pads), key-binding capture, the launcher's SETUP
  rows and `HeaderKeys`. ADL gotcha: a `Key(Item)` / `RestartOnly(Item)` wrapper in `settings::menu` is
  ambiguous with `options::Key` (Item lives in `options`); call the `options::` ones directly.
- Launcher: simple view by default (`[launcher] view = "simple" | "advanced"`, written at once by
  `SetAdvanced` from the saved values, so other edits stay unsaved); its own UP/DOWN and A/Enter handling,
  not ImGui nav (nav lost its focused item, NavId 0, when the window opened behind another one).
- `graphics.hd_textures` has no row anywhere (default on); `gpu/hd_textures.cpp` always uses the HD AI
  Remaster mod's pack when `graphics.hd_remaster` is on.
