#pragma once

// Free third-person camera (experimental; settings [camera] free_camera, off by default).
//
// The game's camera keeps running unchanged: CCameraBehaviourController (camera volumes, script
// cameras, transitions, shake) computes its camera every frame and writes it into the render
// manager's pure3d::VectorCamera. With the free camera on, right after that write the runtime
// replaces the VectorCamera's position and direction with an orbit around the player (right
// stick / mouse, collision against the level). Everything that reads the camera afterwards uses
// the orbit: rendering and culling, and the game's own stick-to-world conversion, so walking is
// relative to the free camera with no other change. Cinematics write the VectorCamera themselves
// (the controller is not updated then), so they show as authored; script cameras hand control
// back too. See camera/free_camera.cpp and runtime/NOTES.md, "Câmera livre".

#include <cstdint>

namespace camera
{
    // The free camera is driving the view now (setting on, in gameplay, not in a cutscene or a
    // script camera). Meanwhile it takes the mouse (input::pc::TakeMouseLook, kernel/pc_input.h)
    // and turns with input::pc::MouseLookDelta; otherwise the mouse moves the right stick as
    // usual. The right stick is read from the game's own controller state (so pads, the keyboard
    // mapping, CRASH_INPUT_SCRIPT and the F5 replay all drive it): it must keep reaching the game.
    bool FreeCameraActive();
}
