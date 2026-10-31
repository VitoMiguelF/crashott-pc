#pragma once

// Performance overlay drawn by the presenter over the game image, plus the frame counters behind
// it. Everything here runs on the command processor thread, from renderer::Swap.
//
// - The overlay shows the frame number (the swap number that CRASH_SCREENSHOTS and
//   screenshot_NNNNN.png use), the input clock (seconds since the game first read the controller,
//   the zero of CRASH_INPUT_SCRIPT), FPS (swaps per second over the last second), the current
//   frame time, min / average / max / 1% low FPS over the last 240 frames (the min FPS is the
//   longest frame's) and a frame-time graph. Its texts follow game.language (settings/ui_text.h).
// - Keys (window.cpp): F1 shows/hides it, F2 marks the current frame ([mark] in the log and a line
//   in <screenshot dir>\marks.txt), F12 saves screenshot_NNNNN.png.
// - Environment: CRASH_OVERLAY=1 starts with it visible, CRASH_SCREENSHOT_OVERLAY=1 keeps it in
//   screenshots (off by default), CRASH_OVERLAY_CORNER=tl|tr|bl|br, CRASH_OVERLAY_SCALE=N,
//   CRASH_FRAME_STATS=<file> writes frame,ms,input_s for every frame (read by tools/run_game.py).
//
// The overlay image is rasterized on the CPU (overlay_font.h) into an upload buffer and copied to
// the back buffer by a pass whose pixel shader reads it as a StructuredBuffer (rhi.h).

#include "rhi.h"
#include "../settings/settings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace gpu::overlay
{
    // `format`: the back buffer's. `outputDir`: where marks.txt goes (the screenshot folder). Returns
    // false (and logs why) if the overlay cannot be drawn; the counters, marks and frame stats still work.
    bool Init(rhi::Device* device, DXGI_FORMAT format, const std::filesystem::path& outputDir);

    // Once per swap, before drawing: frame timing, F1 and F2.
    void BeginFrame(uint64_t frame);
    bool Visible();  // mode other than off
    // Off, FPS only, FPS + frame time (with the graph) or full (frame, input clock, FPS, frame time,
    // graph, internal/output resolution and, in tools/run_game.py runs, the profile and the last
    // button pressed). F1 cycles the modes and saves the choice; the settings screen sets it too.
    void SetMode(settings::OverlayMode mode);
    settings::OverlayMode Mode();
    bool InScreenshots();
    // The back buffer of this present: Draw, DrawImage and DrawBitmap draw into it (width x height).
    void BeginPresent(rhi::Texture* target);
    // Where (0, 0) of the next draws is in the back buffer (BeginPresent: the corner): the game's own
    // 4:3 puts the menus and texts in the picture (gpu/widescreen.h OverlayArea).
    void SetOrigin(int x, int y);
    void Draw(uint32_t width, uint32_t height);
    // Shows "PRINT" in the status line for a moment.
    void NoteScreenshot(uint64_t frame);
    // Shows `text` (upper case ASCII) for a moment in any mode, also with the overlay off (F8).
    void Notice(const std::string& text);

    // Draws an RGBA8 image (R in the low byte, alpha-blended) of at most 512x512 pixels with its
    // top-left corner at (x, y), each pixel a `scale` x `scale` square (the settings screen).
    void DrawImage(uint32_t width, uint32_t height, const uint32_t* pixels, int imageWidth, int imageHeight, int x, int y, int scale);

    // Like DrawImage, any size, 1:1 (the subtitles, subtitles/subtitle_draw.cpp; the settings
    // screen). `version` names the contents: a call with the previous call's version on the same
    // `channel` draws the copy already uploaded and doesn't read `pixels`. Each channel (one per
    // caller, BITMAP_CHANNELS of them: the subtitles, the settings screen, the save screens'
    // keyboard and delete prompt, the cutscenes' skip prompt, the skin challenges' counters, the
    // console, the touch controls'
    // stick, buttons and top row, the MULTIPLAYER screens, the cheat menu, the achievements' screen,
    // popup and hint) has its own upload buffer.
    enum BitmapChannel { BITMAP_SUBTITLES, BITMAP_SETTINGS, BITMAP_SAVE_SCREENS, BITMAP_SKIP_PROMPT, BITMAP_CHALLENGES, BITMAP_CONSOLE,
                         BITMAP_TOUCH_STICK, BITMAP_TOUCH_KNOB, BITMAP_TOUCH_BUTTONS, BITMAP_TOUCH_TOP, BITMAP_MULTIPLAYER,
                         BITMAP_CHEATS,
                         BITMAP_ACHIEVEMENTS, BITMAP_ACHIEVEMENT_POPUP, BITMAP_ACHIEVEMENT_HINT,
                         BITMAP_CHANNELS };
    void DrawBitmap(uint32_t width, uint32_t height, const uint32_t* pixels, int imageWidth, int imageHeight, int x, int y, uint64_t version,
                    BitmapChannel channel = BITMAP_SUBTITLES);
}
