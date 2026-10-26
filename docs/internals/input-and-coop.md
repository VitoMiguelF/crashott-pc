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
