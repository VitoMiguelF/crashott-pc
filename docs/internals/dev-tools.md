# Dev tools: in-game Lua, cheats, console, skins, Lua decompiler

Full write-ups: [docs/dev_features.md](../dev_features.md) (catalogue with risks and follow-ups),
`runtime/NOTES.md` "Console de desenvolvedor e troca de skin"; GUIA "Recursos DEV", "Console de
desenvolvedor". Code: `runtime/dev/`. Profiles `dev_*`, `console_*`.

## Lua in the game

- The Lua compiler is in retail. `luaL_loadbuffer` sub_823C9530, `lua_pcall` sub_822995E8,
  `L = *(*(0x824FBFE8) + 1056)`. Blocks run after `CScriptManager::vf10` sub_82230DE0.
  `CRASH_DEV_LUA` / `CRASH_DEV_LUA_FILE` (lines `@event+s`) / `CRASH_DEV_LUA_AT`.
- Dialect is 5.0-like: no `#`, no `...`. Global `print` is a no-op in retail; the runtime registers `devprint`
  (base print sub_823CCF40 -> fputs sub_823D3410, hooked to the log).
- Radical's `luaG_errormsg` (sub_822A2C28) calls the global `_TRACEBACK` when there is no errfunc. It is absent
  in retail, so every runtime error became "error in error handling" / a crash. The runtime defines it;
  `currentline` sub_822A1DB0 is replaced by a checked version.
- Wrong arguments to native methods still crash (no validation). Static binding list: `AddMethod`
  sub_822D54A8; template RTTI gives the signatures.
- Navigation: `Managers.GameDatabase:GetCurrentPlayer(0)` = actor; `.m_BehaviourController` (a field, not a
  method) -> `.m_DamageableBehaviour` / `.m_PhysicsBehaviour:Teleport(tovector())`. `getobj(":SettingsDatabase")`,
  `getobj(":UberManager")`.
- Objectives: `NewObjective` + `DO_*` + `:Done()`. Limits: ~200 steps never runs, 9 objectives at once crash,
  unnamed enum values crash (skins 18-20, concept art 20-24, enemy info 1).
- Hidden level `SoundTest` (index 20) loads only with a mod supplying `levels\SoundTest\spawn_normal.blua` +
  `cameraoverrides.blua`; black scene.

## Cheats and invincibility

- Invincible: hook `AdjustHitpoints` sub_82110448; behaviour actor +28 vs static `CGameDatabase` 0x824A76A4
  +16 / +20.
- The original X360 cheats work: pause, hold RT, X X Y A = Big Head, Y X Y A = Shadow (checker sub_821DB680,
  settings 0x824A8B80 +93 / +94).
- Unlocked-skins list 0x824A8218 holds 20; Crash_Hulk = ESkin 22 (`dev.unlock_skin`).
- CStatsManager = managers subsystem 16 (sub_821FB4F0 + sub_821FA440). GetValueInt(class, stat) =
  `*(this + 4*(93*class + stat + 114))`; IncrementValueInt sub_82248F88 adds an unsigned 32-bit amount to classes 0
  and 1 unchecked; SetValueInt sub_82247FB8. Stat 21 = MojoCount (enum registration: `li r4,21` before the name
  string at 0x82026C64). Lua numbers are float: pass big values natively. Console `mojo` keeps the total in
  0..2^31-1.

## Developer console

- Off by default: `[dev] console`, `--dev-console`, `CRASH_DEV_CONSOLE=1`. Key = scan code 0x29 (left of 1, any
  layout) or F10. Drawn with GDI Consolas -> `overlay::DrawBitmap` channel `BITMAP_CONSOLE`. Commands run on
  the game thread from the `CScriptManager::vf10` hook (`dev_tools.cpp`).
- Tests: `dev_console = [[time, entry]]` in run.toml posts WM_KEYDOWN / WM_CHAR to the window. Don't post
  WM_CHAR for a key (TranslateMessage makes one -> double char). The toggle key can be a dead key: flushed with
  `ToUnicode(VK_SPACE)`.
- `level` command: in a level, `UberManager:SetLevelIndex` + `ForceReload` (`StartLevelByName` in-game stalls
  the game); in the HUB / main menu, `Frontend:StartLevel` + exit 51 via `dev::console::OverrideExit` in
  `frontend_hooks.cpp`.

## Skins

- Skin = template hash at 0x824A8270; SelectSkin path sub_821D9270. In a level the change is pending and
  applied in the `CStreamManager::vf05` hook (sub_8224E068); `now` = ForceReload. InLevel = vf05..vf04 AND
  player 1 (0x824A76A4+16) non-null (the HUB goes through vf05 too).
- Crash_Hulk fixes: sub_82215730 hash `"Characters:Crash_Hulk"`. Collision: `CPhysicsBehaviour::vf12`
  sub_82147568 / `CIntersectionBehaviour::vf12` sub_8211EA88 get r5 = the stream entry of
  `"Characters:Crash"` (entries at *0x824A90CC +4..+8, hash at +20) + a held reference (sub_8224D3C8);
  without it Crash falls through the floor or vf12 reads null.
- The skin screen preview switch sub_820BBB68 has no case 22 (crashes) -> a hook replicates it (text index 22 =
  "CRASH HULK" exists). `IsSkinUnlocked` sub_82215900 / next-prev sub_822159F8 with `[dev] hulk_skin`.
- A Lua template wrapper does NOT help: the player spawns from the native package template.

## Lua decompiler (`tools/luadec.py`)

- Modes `dis` / `dec` / `all` (-> `private\scripts_dec\*.dec.lua` + `*.dis.txt`, local only) / `check`. All 723
  compiled scripts parse with 0 gotos, and all reconstructions compile in the game's own compiler (profile
  `dev_luadec`, `test = false`).
- Format is the **Lua 5.0 dump** (header `1B Lua 51 01 04 04 04 04`, float test number 3.14159265e7; numbers
  are float; lineinfo u16). Instruction layout 5.0 (OP 0-5, C 6-14, B 15-23, A 24-31); RK constant if >= 250.
- `luaV_execute` sub_8229E7A0 (switch 0x8229E874, 36 ops): 5.0 ops 0-27, then FORLOOP 28, FORPREP 29, TFORLOOP
  30, TFORPREP 31, SETLIST 32, SETLISTO 33, CLOSE 34, CLOSURE 35. For loops are 5.1-style (vars at A+3);
  5.0-style while (condition at the bottom).
- Retail `string.dump` (sub_823CA8A8 -> luaU_dump sub_822A1CF0) crashes in DumpString sub_822A1548 even for
  `loadstring("return 1")`: no bytecode round trip in the game; validate by compiling only.
- Locals debug info is present in retail scripts (names of locals, params, upvalues).
- After changing the decompiler: `luadec.py check` + `run_game.py dev_luadec`.
