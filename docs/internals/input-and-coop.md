# Input, button icons and local co-op

Full write-ups: `runtime/NOTES.md` "Controles de PC" (with "Co-op local" and "Mouse nos menus"); GUIA
"Controles de PC". Profiles: `teclado`, `icones`, `mouse_menus`, `controles_tela`, `coop_p2`;
`coop_jogadores` and `coop_manual` need two real pads (`test = false`).

## Modules

| Module | What |
|---|---|
| `kernel/pc_input.*` | Keyboard + mouse -> controller 0; menus vs gameplay from Frontend.bfig nodes; text-entry mode |
| `kernel/hid_pads.*` | DualSense / DS4 over HID (`Pads()`, `ReadPad(id)`, `SetPadVibration(id)`) |
| `kernel/pad_slots.*` | Which physical device drives which player (co-op) |
| `kernel/stick_rotation_hook.cpp` | Spin moves from the keyboard |
| `settings/controls.*` | `[controls]` in settings.toml |
| `settings/prompt_icons.*` | Our own PlayStation / keyboard button icons |
| `settings/menu_mouse.*` | Mouse in the game's Scrooby menus |
| `settings/touch_controls.*` | On-screen touch pad (Android): gameplay only, the menus take the touch as the mouse |

## Facts

- Game input map = `script\mixins\inputmap_methods.lua`. Input-map button channels move with the control
  scheme: read e.g. R3 through `input::MenuButtons`, not a fixed channel.
- Spin moves need the left stick rotated: detector sub_821EC3E0 (7 of 8 octants within 0.75 s, stick > 60%).
- Button icons are palettized k_8 textures + a 256x1 palette at sampler s8 (`pal*fp` shaders); invisible to
  the RGBA texture dump. Keys = `hd::Key` of the R8 indices.
- Active menu per frame = `CMenuAction::vf05` (sub_820B2E98), this+24 = Menu. `Menu::vf05` also runs for
  hidden menus.
- Mouse menus behave like a PC game (hover selects): hover calls the game's `Menu::SetSelection`
  sub_822F2E40(menu, i) + the move sound sub_821DB260 / 280 on player sub_821FA440(sub_821FB4F0(), 6);
  prev/next sub_822F2F58 / 3068. The settings screen has hit zones (`g_zones`) built in `Rasterize`.
- The game's button prompts are clickable in the menus ("(B) BACK", "CONTINUE STORY (Y)"): the renderer notes
  where each icon is drawn (`NoteIconSpot`, through `HUD_VS`), `settings::prompts::ButtonAt` maps a click on
  the icon or its label to the button, `pc_input.cpp` `LeftClick` presses it for 3 game reads.
- The save screens ignore presses and clicks made before their first frame (they used to open the delete
  question by themselves) and leave clicks off their own zones to the game's menu (a slot click is the game's).
- Test scripts: `KEY_*`, `MOUSE`, `CLICK`, `RCLICK`, `LDOWN`, `LUP`, `WHEEL`, `DEVICE_*`. With scripted mouse
  entries the real mouse over the window is ignored (otherwise your own mouse moves the test cursor).
- DSX / HidHide can hold a Bluetooth DualSense's HID path (access denied); USB reads fine.
- Wall-clock `CRASH_INPUT_SCRIPT` tests are flaky when other game instances run; rerun alone before blaming
  code.

## Local co-op

- The game itself handles co-op: it polls all 4 XamInput users each tick (sub_8228A7C8). A 2nd connected user
  makes the HUD show JOIN GAME (B); B puts P2 in P1's **backpack**; Y (`EInputMapEvent_DETACH`) jumps off, but
  only a few seconds after joining (Y 2.5 s after B does nothing). START on P2 = "P2 PAUSED".
- P1 = the controller that pressed START on the title: sub_821DB680 -> sub_821FC498(managers, index), index at
  managers+348, called only at the title.
- Old bug: HID PlayStation pad n was OR'd into user n together with XInput n, so Xbox + DualSense, or a DSX
  virtual pad + the visible DualSense, all drove P1.
- `pad_slots.*`: device table (XInput slot / stable HID id / mirror pairs detected by coincident presses),
  sticky players, `[controls] player1 / player2` (`"auto"` | `xboxN` | `playstationN`), keyboard on
  `PlayerOneUser()` (hooked sub_821FC498), a single pad = P1. `--pads-selftest`.
- Tests: `P2_<button|LS|RS|LT|RT>` in `CRASH_INPUT_SCRIPT` / run.toml = a virtual 2nd pad.
- Lua `GetCurrentPlayer(1)` returns P1; useless for P2's position.
- Start any input / co-op change from `pad_slots.cpp`.

## Controller mapping (`[controls] pad_layout`)

- One row in CONTROLS (`options::Item::ControllerLayout`, 4th, after BUTTON ICONS): XBOX 360 (the game's) or
  PS2. `controls.cpp` tables `PS2_GAMEPLAY` / `PS2_MENU` `[game button] = physical button`
  (`settings::PadButton`: A B X Y LB RB LT RT LS RS START BACK and the D-pad; the triggers count as buttons).
  PS2 gameplay: RB (R1) = the game's RT (block), RT (R2) = the game's RB (free kick). PS2 menus: Y = the
  game's B (back / leave, as on the PS2), B = the game's Y (on the HUB: continue story).
- `settings::PadScreenOf(node)`: 638 GameRunning and 1170 TutorialScreen (the hints, opened over the level by
  exit 640; as a menu they swapped B / Y and the jack icon over the enemies showed Triangle) = gameplay; the cutscene nodes (633, 655-1051) and no node
  yet = no change (B keeps skipping); any other node = a menu (pause included). `input::MenuButtons` (the
  runtime's own screens) goes through `RemapPad` too, so Y goes back there as well; the keyboard's Esc stays B.
- Applied in `kernel/xam_input_imports.cpp` `RemapPad`, right after `input::slots::Read` (every physical pad,
  every player) and on each `CRASH_INPUT_SCRIPT` press. The keyboard (`pc_input`) and the touch controls are
  added after it and are not remapped. Logs `[input] controller mapping: ps2, gameplay (...)` / `ps2, menus (...)` on each change.
- Icons: `prompt_icons.cpp` `MappedIcon` swaps the **PlayStation** icons on those screens (the style hash
  includes the screen, so the renderer redraws them). The Xbox icons are the game's own textures and stay.
  The pause screens (1081 / 1109 / 1134) swap the buttons but keep the gameplay icons: they have no B / Y
  prompts of their own, and the level drawn behind them does (the B floating over a hint stone turned into
  Triangle). The screen is fixed when `Style()` makes the style (`g_styleScreens`) and read back when drawing:
  deciding it again at draw time raced with the node change when leaving the pause, and the wrong icon stayed
  cached under the gameplay style (the B over the stone stayed Triangle in the level). The skip prompt
  (`skip_cinematics.cpp`) shows START instead of B with PS2, the logo videos included; the buttons don't
  swap there (nodes < 59 and the cutscenes), so B still skips too.
- The new row moved the key bindings down one: `controles_tela` now presses DOWN x14 to reach JUMP.
- 2026-10-31: WALK SPEED and MOUSE IN MENUS left the tab (always 40% / always on; old `walk_speed` /
  `mouse_menus` keys are ignored silently) and VIBRATION came in after CONTROLLER MAPPING: JUMP is DOWN x13.
  `[controls] vibration = false` zeroes the motors in `input::slots::SetVibration`, the one path from
  `XamInputSetState` to every pad backend (XInput, HID DualSense/DS4, SDL).
- Profile `mapeamento_controle` (only checks the row and the log; the PS2 buttons were tested by hand).

## Light bar (`kernel/pad_lights.*`, `[controls] lightbar`)

- LIGHT BAR row after VIBRATION in the CONTROLS tab: `controles_tela` presses DOWN x14 to reach JUMP.
- `input::lights::Tick` (game thread, from the `XamInputGetState` poll of player 1, every 30 ms) -> `input::slots::SetLight(user, rgb, player)` -> `hid::SetPadLight` (only a PlayStation pad on its own: a Mirror pad is DSX/DS4Windows'). Win32: one output report carries motors + light (`SendOutput`); DualSense gets a LIGHTBAR_SETUP light-out report first. Offsets (Linux hid-playstation): DualSense common part at byte 1 (USB 0x02) / 3 (BT 0x31, seq, tag 0x10): flags +0/+1 (0x04 lightbar, 0x10 player LEDs), motors +2/+3, flag2 +38, setup +41, player LEDs +43, RGB +44..46. DS4 common part at 1 (USB 0x05) / 3 (BT 0x11, 0xC0): flags +0 (0x01 motors, 0x02 light), motors +3/+4, RGB +5..7. BT ends with CRC-32 seeded with 0xA2. SDL: `SDL_SetGamepadLED` + player index.
- DSX / DS4Windows: the hid scan thread looks for `dsx.exe` / `dualsensex.exe` / `ds4windows.exe` every 6 s; while one runs nothing is sent.
- Game state: CGameDatabase 0x824A76A4 +16/+20 = actor each player controls, +24/+28 = the other actor of a jack (StartJacking `sub_82120C08` sets, EndJacking `sub_82120D40` clears); the one that is not the Crash's actor (Crash +0xDC physics +96) is the titan. Titan type = its fight tree behaviour's sub type hash at +32 (`IsSubType` `sub_821199C0`; hash `sub_822845A0`: h = h * 65599 ^ c) among the actor's behaviours (pointers in its behaviour controller, actor +0x1C, scanned like mp_net's FindDamageable; behaviour actor +28). Sub types: Spike, Shurtle, Sludge, Ratcicle, Battler, Grizzly, Roller, Parafox, Stinky, Eelectric, Shellephant, Scorporilla, Yuktopus, UkaUkaBoss, NinaSpiderBot (= EStatInt 30-44 `*Jacked`). Player -> game controller: CFrontendManager (manager 6) +16580 [2]. KO = the Crash's CDamageableBehaviour (0x8201145C) hitpoints 0; pause = in a level and `pc::InMenus()`.
- Colours: `mp::colors::ForSlot` (online slot, else the player index) -> LED table; orange + a skin = `mp::colors::SkinSwatch` (the costume's hue from the build-time skin table). `CRASH_LIGHTBAR_TRACE=1` logs the database words on each jack.
