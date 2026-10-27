#pragma once

// Keyboard and mouse as controller 0, and the last device used (pc_input.cpp).
//
// The window (gpu/window.cpp) reports keys, mouse buttons, the wheel, the cursor and raw mouse
// motion here; xam_input_imports.cpp asks for the controller 0 they make each time the game reads
// it. Two contexts:
// - gameplay: the [controls] bindings (settings/controls.h): WASD = left stick, mouse / arrows =
//   right stick (camera), Space = A, mouse 1 = X, mouse 2 = Y, E = B, Shift = RT, Esc = START...
//   RotateStick turns the left stick in circles (one turn every 9 game reads, 0.3 s at 30 Hz: the
//   game's detector wants 7 of the 8 octants within 0.75 s with the stick past 60 %), and
//   SpinAttack does that while tapping X (Crash's spin, "rotate + repeatedly press light attack").
// - menus (the frontend's screens, which the game's Frontend fight tree shows; see InMenus): fixed
//   keys (arrows / WASD = D-pad, Enter / Space = A, Esc = B, Backspace = X, Tab = Y, P = START,
//   Q / E = LB / RB) and the mouse (menu_mouse.cpp: pointing at an item selects it at once, through
//   the game's own Menu::SetSelection, a click accepts it, the right button goes back).
//
// The camera mod and anything else that wants the mouse itself: TakeMouseLook / MouseLookDelta.

#include "xinput_types.h"
#include "../platform/keys.h"

#include <cstdint>
#include <string>
#include <vector>

namespace input::pc
{
    // Startup (after settings::Load and vfs::Init): reads which Frontend.bfig nodes are gameplay,
    // starts the PlayStation pad scan.
    void Init();

    // ---- window thread (gpu/window.cpp) ----
    // A key or mouse button (VK_*; left/right Shift, Ctrl and Alt as VK_L*/VK_R*) went down or up.
    void OnKey(uint32_t key, bool down);
    // Wheel notches (+ = away from the user).
    void OnWheel(int notches);
    // Cursor position in client pixels, and the client size.
    void OnCursor(int x, int y, int clientWidth, int clientHeight);
    // Raw mouse motion (counts), whatever the cursor does.
    void OnRawMouse(int dx, int dy);
    // The window gained / lost the keyboard focus (lost: every key is released).
    void OnFocus(bool focused);
    // A key (VK_*) held now, and any keyboard key (VK_BACK and up) held now, as the window saw
    // them (all released while the window has no focus). For GetAsyncKeyState-style polling.
    bool KeyHeld(uint32_t key);
    bool AnyKeyboardKeyHeld();
    // What the window should do with the cursor now: show it, or hide it and keep it inside the
    // window (gameplay with the mouse camera). Re-evaluated by the window ~10 times a second.
    enum class CursorMode : uint8_t { Visible, Hidden, Captured };
    CursorMode WantedCursor();

    // ---- game thread ----
    // Adds the keyboard and the mouse to controller 0's `state` (the pad's reading, already in
    // it). `poll`: the game's own read (XamInputGetState), which advances the stick rotation and
    // eats the mouse motion. `scriptKeys`: keys held by CRASH_INPUT_SCRIPT / the profile's input.
    void Apply(XINPUT_STATE& state, bool poll, const std::vector<uint16_t>& scriptKeys);
    // Controller 0's menu keys only (the runtime's settings screen).
    uint16_t MenuKeys(const std::vector<uint16_t>& scriptKeys);
    // The frontend is showing a menu (main menu, pause, save slots...), not gameplay or a cutscene.
    bool InMenus();
    // The app went to the background (gpu/window.h, Suspended): in gameplay, the game's next reads
    // get START, so it comes back on its pause menu. Pending until that press is over (or the game
    // turned out not to be in gameplay); the window holds the game only after it.
    void RequestBackgroundPause();
    bool BackgroundPausePending();
    void CancelBackgroundPause();  // the game didn't read its pad in time: no press after the return
    // The game's Frontend tree node shown now (frontend_hooks.cpp, every frame it runs).
    void NoteFrontendNode(uint32_t node);

    // Key capture for the settings screen's CONTROLS tab: the next key or mouse button that goes
    // down is kept for CapturedKey (once) instead of being a press.
    void BeginKeyCapture();
    void CancelKeyCapture();
    bool CapturedKey(uint16_t& key);
    // A CRASH_INPUT_SCRIPT key went down (it is captured too, like a real one).
    void ScriptKeyDown(uint16_t key);

    // Typing (the name entry's on-screen keyboard, settings/save_screens.cpp): while on, the
    // letters A-Z and the digits (top row and keypad) go down as text instead of menu keys
    // (WASD, Q / E, P are not the D-pad, LB / RB, START then), and Enter is '\r' (confirm) instead
    // of A. Arrows, Space (A), Esc (B), Backspace / Delete (X) and Tab (Y) stay menu keys.
    void SetTextEntry(bool on);
    bool TextEntry();
    // The characters typed since the last call ('A'-'Z', '0'-'9', '\r'), real keys and script keys.
    std::u32string TakeTypedText();

    // ---- last device used, for the button icons ----
    enum class Device : uint8_t { Xbox, PlayStation, Keyboard };
    // A pad of that kind moved (xam_input_imports.cpp: XInput pads; hid_pads.cpp: PlayStation).
    void NoteDevice(Device device);
    // The icons to show: controls.prompts, or with "auto" the device used last.
    Device PromptDevice();
    const char* DeviceName(Device device);  // "xbox", "playstation", "keyboard"
    // Incremented whenever PromptDevice changes (texture swaps watch it).
    uint32_t PromptGeneration();

    // ---- the mouse in menus (settings/menu_mouse.cpp: the game's menus; settings_menu.cpp) ----
    // The cursor on the 1280x720 game image (the image is fitted into the window and centred);
    // false outside it or with no cursor yet.
    bool CursorOnImage(float& x, float& y);
    // The cursor in the window's client area, 0..1 (the settings screen maps it itself).
    bool CursorInClient(float& u, float& v);
    // Incremented whenever the cursor moves (hover reacts to motion only).
    uint32_t CursorMoves();
    // Left clicks so far, and whether the left button is held now.
    uint32_t LeftClicks();
    bool LeftHeld();
    // A left click for the game's menus not yet used (and at most 400 ms old); taken once.
    bool TakeMenuClick();
    // Wheel notches for the game's menus / the settings screen since the last call (+ = up).
    int TakeMenuWheel();
    int TakeSettingsWheel();
    // The game's menus: press A for the next two game reads (a click on an item).
    void PressMenuAccept();

    // CRASH_INPUT_SCRIPT MOUSE=x/y, CLICK=x/y, RCLICK=x/y (tests): the cursor at (x, y) of the
    // 1280x720 game image, with a left (1) or right (2) click or none (0); LDOWN=x/y (3) and
    // LUP=x/y (4) press / release the left button there (drags); WHEEL=n (5): n notches (+ = up).
    void ScriptMouse(float x, float y, int click);
    // The script has mouse entries: the real mouse is ignored over the window from now on.
    void UseScriptedMouse();

    // ---- mouse look for other modules (the third-person camera mod) ----
    // While taken, mouse motion no longer moves the right stick; the owner reads it itself.
    void TakeMouseLook(bool take);
    // Raw counts since the last call (both 0 when the mouse camera is off, in menus or unfocused).
    void MouseLookDelta(float& dx, float& dy);
}
