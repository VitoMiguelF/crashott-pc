// Skipping the cutscenes and the intro videos (skip_cinematics.h; docs/GUIA_DO_PROJETO.md "Pular cenas e
// videos", runtime/NOTES.md "Pular as cenas").
//
// A cutscene is a CMasterCinematicAction (the action of a fight tree MasterCinematicTrack, e.g. the
// Frontend.bfig node 756 R1_NIS01 of L1_E1) with a state machine at +412, changed only by
// SetState (sub_821B92E8(action, state)) and run by its update (vf05, sub_821BA100):
//
//   0 prepare -> 1 or 2      1 fade in (preCinematicFade)    2 start (load the camera/sounds)
//   3 playing: the camera action (+340) and the sound sequence (+336) drive the time, in
//     frames at [action+12]+124 (the sound stream's position while it plays, so the clock of the
//     game can't make it faster); when both are done (or the managers' "profile changed" bit,
//     +352 & 0x80, is set) -> 4
//   4 end -> 5 or 6          5 fade out (postCinematicFade)  6 post tracks (+236), player shown
//   7 done: action+418 |= 0x80 and [managers 12]+452 |= 0x80, which the frontend's ExitWhenDone
//     condition waits for (forcing that exit instead leaves the level on LOADING forever)
//
// Skipping calls SetState(4) itself while the cutscene plays: the same path its end takes (sound
// stopped, fade, post tracks, done flag), so the frontend leaves the NIS node by itself.
//
// The videos: CMoviePlayerBehaviour::PlayMovie(index) (sub_8214ABD0, hooked in direct_boot.cpp,
// which calls NoteMovie) starts one, its callback (CMovieCallback::vf00, sub_8214B210) runs when it
// ends and starts the next. The game skips a video on a START press: the skip presses it.

#include "skip_cinematics.h"
#include "direct_boot.h"
#include "menu_art.h"
#include "prompt_icons.h"
#include "settings.h"
#include "../gpu/overlay.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "../subtitles/subtitles.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

PPC_EXTERN_FUNC(sub_821B92E8);
extern "C" PPC_FUNC(__imp__sub_821B92E8);  // CMasterCinematicAction::SetState(state)
extern "C" PPC_FUNC(__imp__sub_821BA100);  // CMasterCinematicAction::vf05 (update)
extern "C" PPC_FUNC(__imp__sub_8214B210);  // CMoviePlayerBehaviour::CMovieCallback::vf00 (a video ended)

namespace
{
    constexpr uint32_t ACTION_STATE = 412, ACTION_TRACK = 12, TRACK_TIME = 124;
    constexpr uint32_t STATE_PLAYING = 3, STATE_END = 4, STATE_DONE = 7;
    constexpr int MOVIE_SIERRA = 2, MOVIE_ATTRACT = 4;  // Sierra: missing from the discs, fails at once

    // Frontend.bfig nodes of the cutscenes (direct_boot.cpp's NODE_LEVEL_INTRO and NODE_CINEMATICS..END):
    // a master cinematic action that plays while the frontend is elsewhere (638 GameRunning) is a
    // scripted sequence inside the gameplay (L5_E1 loops some every 5-8 s), not a cutscene.
    constexpr uint32_t NODE_LEVEL_INTRO = 633, NODE_CINEMATICS = 655, NODE_CINEMATICS_END = 1051;

    // A cutscene is playing while its update ran in state 3 this recently.
    constexpr ULONGLONG PLAYING_GRACE_MS = 400;
    // Hold this long to skip.
    constexpr ULONGLONG HOLD_MS = 800;
    // The prompt: fade in, shown this long after the start or the last input, fade out.
    constexpr ULONGLONG FADE_IN_MS = 250, SHOWN_MS = 4000, FADE_OUT_MS = 500;
    // START presses (game reads) that skip a video.
    constexpr int VIDEO_SKIP_READS = 4;
    constexpr WORD SKIP_BUTTONS = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START;

    enum class Kind : int { None, Cutscene, Video };

    std::atomic<uint32_t> g_action{ 0 };         // the playing CMasterCinematicAction
    std::atomic<ULONGLONG> g_actionSeen{ 0 };    // GetTickCount64 of its last update in state 3
    std::atomic<uint32_t> g_skipAction{ 0 };     // the hold asked to end this one (its next update does)
    std::atomic<int> g_movie{ -1 };              // the video playing, -1 none
    std::atomic<ULONGLONG> g_movieStart{ 0 };
    std::atomic<uint32_t> g_movieCount{ 0 };    // videos started so far (each one gets its prompt)
    std::atomic<uint32_t> g_frontendNode{ UINT32_MAX }, g_movieNode{ UINT32_MAX };  // the screen shown, and when the video started

    // Shared with the render thread.
    std::atomic<int> g_kind{ int(Kind::None) };  // what the prompt is for
    std::atomic<ULONGLONG> g_kindSince{ 0 };     // GetTickCount64 it started
    std::atomic<ULONGLONG> g_lastInput{ 0 };     // the player touched something (the prompt comes back)
    std::atomic<float> g_progress{ 0.0f };       // the hold, 0..1
    std::atomic<bool> g_enabled{ true };         // game.skip_cutscenes
    // The prompt's rectangle in the window, 0..1 (render thread -> the mouse on the game thread).
    std::atomic<float> g_hitX0{ 0 }, g_hitY0{ 0 }, g_hitX1{ 0 }, g_hitY1{ 0 };

    float CinematicFrames(uint32_t action)
    {
        uint32_t track = LoadBE32(action + ACTION_TRACK);
        if (!track)
            return 0.0f;
        uint32_t bits = LoadBE32(track + TRACK_TIME);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    // CRASH_SKIP_CINEMATICS: updates of the cutscene in state 3 before it is skipped.
    int SkipAfterUpdates()
    {
        static int value = [] {
            const char* text = getenv("CRASH_SKIP_CINEMATICS_AFTER");
            return text && *text ? std::max(1, atoi(text)) : 1;
        }();
        return value;
    }

    uint32_t g_autoAction = 0;
    int g_autoUpdates = 0;

    Kind CurrentKind(ULONGLONG now)
    {
        if (g_action.load() && now - g_actionSeen.load() < PLAYING_GRACE_MS)
            return Kind::Cutscene;
        // (the attract video, shown when the title screen is left alone, keeps the game's own
        // any-button exit)
        int movie = g_movie.load();
        if (movie >= 0 && movie != MOVIE_SIERRA && movie != MOVIE_ATTRACT)
            return Kind::Video;
        return Kind::None;
    }

    // Enter (not a game button during a cutscene) and the mouse on the prompt, from the window.
    bool KeyboardOrMouseHeld()
    {
        HWND foreground = GetForegroundWindow();
        DWORD process = 0;
        if (!foreground || (GetWindowThreadProcessId(foreground, &process), process != GetCurrentProcessId()))
            return false;
        if (GetAsyncKeyState(VK_RETURN) & 0x8000)
            return true;
        float u, v;
        if (input::pc::LeftHeld() && input::pc::CursorInClient(u, v))
            return u >= g_hitX0.load() && u <= g_hitX1.load() && v >= g_hitY0.load() && v <= g_hitY1.load() && g_hitX1.load() > 0.0f;
        return false;
    }

    // The keyboard keys that hold (Esc / Space / Enter come through the game's controller 0 as
    // START / A / B; Enter is read here).
    bool AnyKeyDown()
    {
        HWND foreground = GetForegroundWindow();
        DWORD process = 0;
        if (!foreground || (GetWindowThreadProcessId(foreground, &process), process != GetCurrentProcessId()))
            return false;
        for (int key = 8; key < 256; key++)
        {
            if (GetAsyncKeyState(key) & 0x8000)
                return true;
        }
        return false;
    }
}

namespace cutscenes
{
    void NoteMovie(int index)
    {
        g_movie = index;
        g_movieStart = GetTickCount64();
        g_movieCount++;
        g_movieNode = g_frontendNode.load();
        Log("[cutscene] video %d starts (frontend node %d)", index, int(g_movieNode.load()));
    }

    void NoteFrontendNode(uint32_t node)
    {
        if (g_frontendNode.exchange(node) == node)
            return;
        // A video doesn't outlive the screen that played it (in case its end callback never came).
        if (g_movie.load() >= 0 && g_movieNode.load() != node)
        {
            Log("[cutscene] video %d: the frontend moved on to node %u", g_movie.load(), node);
            g_movie = -1;
        }
    }

    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll)
    {
        static WORD heldMask = 0;        // skip buttons hidden until released (after a skip)
        static ULONGLONG holdStart = 0;
        static uint64_t shown = 0;  // what the prompt is for: the kind and which video / cutscene
        static int videoStartReads = 0;
        static XINPUT_GAMEPAD previous{};
        static uint32_t cursorMoves = 0;
        static int settingsReads = 0;
        if (!poll)
        {
            // Other reads (the settings screen...): the game must not see the hidden buttons either.
            if (CurrentKind(GetTickCount64()) != Kind::None && g_enabled.load())
                pad.wButtons &= ~SKIP_BUTTONS;
            pad.wButtons &= ~heldMask;
            return;
        }
        if (settingsReads-- <= 0)
        {
            settingsReads = 15;
            g_enabled = settings::Get().skipCutscenes;
        }
        ULONGLONG now = GetTickCount64();
        Kind kind = g_enabled.load() ? CurrentKind(now) : Kind::None;

        // Input of any kind brings the prompt back.
        bool touched = pad.wButtons != previous.wButtons || std::abs(pad.sThumbLX - previous.sThumbLX) > 8000 ||
                       std::abs(pad.sThumbLY - previous.sThumbLY) > 8000 || std::abs(pad.sThumbRX - previous.sThumbRX) > 8000 ||
                       std::abs(pad.sThumbRY - previous.sThumbRY) > 8000 || pad.bLeftTrigger != previous.bLeftTrigger ||
                       pad.bRightTrigger != previous.bRightTrigger || input::pc::CursorMoves() != cursorMoves;
        previous = pad;
        cursorMoves = input::pc::CursorMoves();
        if (kind != Kind::None && (touched || AnyKeyDown()))
            g_lastInput = now;

        uint64_t id = kind == Kind::Video ? (uint64_t(1) << 32 | g_movieCount.load())
                    : kind == Kind::Cutscene ? (uint64_t(2) << 32 | g_action.load()) : 0;
        if (id != shown)
        {
            shown = id;
            holdStart = 0;
            g_progress = 0.0f;
            g_kindSince = now;
            g_kind = int(kind);
            if (kind != Kind::None)
                boot::Event("skip_prompt", kind == Kind::Video ? "video" : "cutscene");
        }

        const WORD raw = pad.wButtons;  // before anything is hidden
        bool held = (raw & SKIP_BUTTONS) != 0 || KeyboardOrMouseHeld();
        if (kind != Kind::None)
        {
            if (held && !(heldMask && (raw & heldMask)))
            {
                if (!holdStart)
                    holdStart = now;
                g_progress = std::min(1.0f, float(now - holdStart) / float(HOLD_MS));
                g_lastInput = now;
                if (now - holdStart >= HOLD_MS)
                {
                    char detail[64];
                    if (kind == Kind::Cutscene)
                    {
                        uint32_t action = g_action.load();
                        snprintf(detail, sizeof(detail), "held, cutscene frame %.1f", CinematicFrames(action));
                        g_skipAction = action;
                    }
                    else
                    {
                        snprintf(detail, sizeof(detail), "held, video %d", g_movie.load());
                        videoStartReads = VIDEO_SKIP_READS;
                    }
                    boot::Event(kind == Kind::Cutscene ? "cinematic_skipped" : "video_skipped", detail);
                    subtitles::EndCutscenes();
                    heldMask = SKIP_BUTTONS;
                    holdStart = 0;
                    g_progress = 0.0f;
                }
            }
            else
            {
                holdStart = 0;
                g_progress = 0.0f;
            }
            // The game doesn't see them meanwhile (a video would skip at the first press).
            pad.wButtons &= ~SKIP_BUTTONS;
        }
        if (heldMask)
        {
            if (!(raw & heldMask) && !KeyboardOrMouseHeld())
                heldMask = 0;
            pad.wButtons &= ~heldMask;
        }
        if (videoStartReads > 0)
        {
            videoStartReads--;
            pad.wButtons |= XINPUT_GAMEPAD_START;  // the game's own skip
        }
        // CRASH_SKIP_TRACE=1: every game read while a prompt is up or the buttons are hidden, and the
        // 3 s after.
        static const bool trace = getenv("CRASH_SKIP_TRACE") && strcmp(getenv("CRASH_SKIP_TRACE"), "0") != 0;
        static ULONGLONG traceUntil = 0;
        if (trace && (kind != Kind::None || heldMask || held))
            traceUntil = now + 3000;
        if (trace && now < traceUntil)
            Log("[cutscene] read: kind %d, held %d, hold %llu ms, hidden 0x%04X, game sees 0x%04X", int(kind), int(held),
                holdStart ? (unsigned long long)(now - holdStart) : 0ull, heldMask, pad.wButtons);
    }
}

// SetState: the transitions in the log, the events of the end.
PPC_FUNC(sub_821B92E8)
{
    uint32_t action = ctx.r3.u32;
    uint32_t from = LoadBE32(action + ACTION_STATE), to = ctx.r4.u32;
    __imp__sub_821B92E8(ctx, base);
    // SetState goes on through states 0 and 4 by itself (they pick the next one): the final state.
    uint32_t now = LoadBE32(action + ACTION_STATE);
    // (the first call, from the start action, finds the state not set yet)
    Log("[cinematic] 0x%08X state %s -> %u%s (frame %.1f, tick %lld)", action, from <= STATE_DONE ? std::to_string(from).c_str() : "new", to,
        now != to ? (" -> " + std::to_string(now)).c_str() : "", CinematicFrames(action), (long long)input::GameTick());
    if (now != STATE_PLAYING && g_action.load() == action)
        g_action = 0;
    if (now == STATE_DONE)
        boot::Event("cinematic_done");
}

// vf05 (update): the cutscene playing (the prompt), and its end when skipped.
PPC_FUNC(sub_821BA100)
{
    uint32_t action = ctx.r3.u32;
    uint32_t node = g_frontendNode.load();
    bool cutsceneNode = node == NODE_LEVEL_INTRO || (node >= NODE_CINEMATICS && node <= NODE_CINEMATICS_END);
    if (LoadBE32(action + ACTION_STATE) == STATE_PLAYING && cutsceneNode)
    {
        g_action = action;
        g_actionSeen = GetTickCount64();
        g_movie = -1;  // a video can't be playing under a cutscene
        bool skip = g_skipAction.load() == action;
        if (!skip && boot::SkipCinematics())
        {
            if (action != g_autoAction)
                g_autoAction = action, g_autoUpdates = 0;
            if (++g_autoUpdates >= SkipAfterUpdates())
            {
                char detail[64];
                snprintf(detail, sizeof(detail), "frame %.1f", CinematicFrames(action));
                boot::Event("cinematic_skipped", detail);
                subtitles::EndCutscenes();
                skip = true;
            }
        }
        if (skip)
        {
            g_skipAction = 0;
            g_autoAction = 0;
            g_action = 0;
            PPCContext call = ctx;
            call.r3.u64 = action;
            call.r4.u64 = STATE_END;
            sub_821B92E8(call, base);
        }
    }
    __imp__sub_821BA100(ctx, base);
}

// A video ended (or failed to open): the next one, if any, starts from here (PlayMovie).
PPC_FUNC(sub_8214B210)
{
    int movie = g_movie.exchange(-1);
    if (movie >= 0)
        Log("[cutscene] video %d ends (after %.1f s)", movie, double(GetTickCount64() - g_movieStart.load()) / 1000.0);
    __imp__sub_8214B210(ctx, base);
}

// The prompt -------------------------------------------------------------------------------------

namespace
{
    namespace menu_art = settings::menu_art;
    using menu_art::Color;
    using menu_art::FontId;

    constexpr menu_art::TextStyle TEXT{ { 238, 226, 196, 255 }, { 52, 28, 4, 255 } };  // the game's message text
    constexpr Color PLATE{ 14, 8, 0, 165 }, EDGE{ 236, 222, 190, 150 }, RING{ 232, 160, 0, 255 }, RING_TRACK{ 236, 222, 190, 70 };

    // Premultiplied image of an antialiased shape given by a signed distance (negative inside).
    template <typename Distance>
    menu_art::Image Shape(int w, int h, Distance distance, Color fill, Color edge, float edgeWidth)
    {
        menu_art::Image image;
        image.w = w, image.h = h;
        image.px.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                float d = distance(x + 0.5f, y + 0.5f);
                float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cover <= 0.0f)
                    continue;
                float t = edgeWidth > 0.0f ? std::clamp(-d - edgeWidth + 0.5f, 0.0f, 1.0f) : 1.0f;  // 0 on the edge, 1 inside
                float a = (edge.a * (1.0f - t) + fill.a * t) / 255.0f * cover;
                float r = edge.r * (1.0f - t) + fill.r * t, g = edge.g * (1.0f - t) + fill.g * t, b = edge.b * (1.0f - t) + fill.b * t;
                auto p = [&](float c) { return uint32_t(std::lround(c * a)); };
                image.px[size_t(y) * w + x] = p(r) | p(g) << 8 | p(b) << 16 | uint32_t(std::lround(a * 255.0f)) << 24;
            }
        }
        return image;
    }

    menu_art::Image RoundedRect(int w, int h, float radius, Color fill, Color edge, float edgeWidth)
    {
        return Shape(w, h, [&](float x, float y) {
            float qx = std::fabs(x - w * 0.5f) - (w * 0.5f - radius), qy = std::fabs(y - h * 0.5f) - (h * 0.5f - radius);
            float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
        }, fill, edge, edgeWidth);
    }

    // A ring of `size` px, `thickness` thick, filled clockwise from the top up to `progress`.
    menu_art::Image Ring(int size, float thickness, float progress, Color color, Color track)
    {
        menu_art::Image image;
        image.w = image.h = size;
        image.px.assign(size_t(size) * size, 0);
        const float c = size * 0.5f, outer = c - 0.5f, inner = outer - thickness;
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                float dx = x + 0.5f - c, dy = y + 0.5f - c;
                float r = std::sqrt(dx * dx + dy * dy);
                float cover = std::clamp(outer - r + 0.5f, 0.0f, 1.0f) * std::clamp(r - inner + 0.5f, 0.0f, 1.0f);
                if (cover <= 0.0f)
                    continue;
                float angle = std::atan2(dx, -dy);  // 0 at the top, clockwise
                if (angle < 0.0f)
                    angle += 6.2831853f;
                Color k = angle <= progress * 6.2831853f ? color : track;
                float a = k.a / 255.0f * cover;
                auto p = [&](uint8_t v) { return uint32_t(std::lround(v * a)); };
                image.px[size_t(y) * size + x] = p(k.r) | p(k.g) << 8 | p(k.b) << 16 | uint32_t(std::lround(a * 255.0f)) << 24;
            }
        }
        return image;
    }

    struct Look
    {
        int kind = 0;
        bool portuguese = false;
        input::pc::Device device = input::pc::Device::Xbox;
        uint32_t style = 0;
        int progress = 0;  // 0..64
        int width = 0, height = 0;
        bool operator==(const Look&) const = default;
    };

    struct Drawn
    {
        std::vector<uint32_t> pixels, faded;
        int w = 0, h = 0, x = 0, y = 0;
        uint64_t version = 0;
    };

    // Rasterizes the prompt: a dark plate with the button (a ring around it fills while held) and
    // "SKIP SCENE" in the game's font, at the bottom right of a `width` x `height` image.
    void Rasterize(const Look& look, Drawn& out)
    {
        const float s = look.height / 720.0f;
        const float cell = 26.0f * s, iconH = 30.0f * s, ringSize = std::ceil(44.0f * s), pad = 10.0f * s, gap = 10.0f * s;
        const char* text = look.kind == int(Kind::Video) ? (look.portuguese ? "PULAR V\xC3\x8D" "DEO" : "SKIP VIDEO")
                                                         : (look.portuguese ? "PULAR CENA" : "SKIP SCENE");
        std::u32string label = menu_art::Utf32(text);
        float textW = menu_art::TextWidth(FontId::Large, label, cell);
        bool keyboard = look.device == input::pc::Device::Keyboard;
        std::u32string keyText = menu_art::Utf32("ESC");
        float keyTextW = menu_art::TextWidth(FontId::Small, keyText, cell * 0.8f);
        float buttonW = keyboard ? std::max(ringSize, keyTextW + 16.0f * s) : ringSize;
        int w = int(std::ceil(pad + buttonW + gap + textW + pad * 1.4f)), h = int(std::ceil(ringSize + pad * 1.2f));

        menu_art::Canvas canvas;
        canvas.Reset(w, h);
        canvas.Blit(RoundedRect(w, h, h * 0.5f, PLATE, EDGE, std::max(1.0f, 1.5f * s)), 0, 0);
        float bx = pad, by = (h - ringSize) * 0.5f;
        float progress = look.progress / 64.0f;
        if (keyboard)
        {
            // A key cap with ESC, its border filling in place of the ring.
            int kw = int(std::lround(buttonW)), kh = int(std::lround(ringSize * 0.78f));
            int ky = int(std::lround((h - kh) * 0.5f));
            Color border = progress > 0.0f ? RING : Color{ 236, 222, 190, 255 };
            canvas.Blit(RoundedRect(kw, kh, 6.0f * s, Color{ 236, 222, 190, 235 }, border, std::max(2.0f, 3.0f * s * (0.6f + progress))),
                        int(std::lround(bx)), ky);
            if (progress > 0.0f)
            {
                // The fill: a bar along the bottom of the key.
                int fw = int(std::lround((kw - 8.0f * s) * progress));
                canvas.Fill(int(bx + 4.0f * s), ky + kh - int(5.0f * s), int(bx + 4.0f * s) + fw, ky + kh - int(2.0f * s), RING);
            }
            menu_art::TextStyle keyStyle{ { 52, 28, 4, 255 }, { 236, 222, 190, 0 } };
            menu_art::DrawText(canvas, FontId::Small, keyText, bx + (kw - keyTextW) * 0.5f, ky + (kh - cell * 0.8f) * 0.5f, cell * 0.8f, keyStyle);
        }
        else
        {
            int ring = int(ringSize);
            canvas.Blit(Ring(ring, std::max(2.0f, 3.5f * s), progress, RING, RING_TRACK), int(std::lround(bx)), int(std::lround(by)));
            float iconW = menu_art::IconWidth(menu_art::ICON_B, iconH);
            menu_art::DrawIcon(canvas, menu_art::ICON_B, bx + (ringSize - iconW) * 0.5f, by + (ringSize - iconH) * 0.5f, iconH);
        }
        menu_art::DrawText(canvas, FontId::Large, label, bx + buttonW + gap, (h - cell) * 0.5f, cell, TEXT);
        canvas.Export(out.pixels);
        out.w = w, out.h = h;
        // Bottom right, in the letterbox bar of the cutscenes.
        out.x = look.width - w - int(std::lround(36.0f * s));
        out.y = look.height - h - int(std::lround(26.0f * s));
        out.version++;
    }
}

namespace cutscenes
{
    void Draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t width, uint32_t height)
    {
        static Drawn drawn;
        static Look drawnLook;
        static uint64_t fadedVersion = 0;
        Kind kind = Kind(g_kind.load());
        ULONGLONG now = GetTickCount64();
        if (kind == Kind::None || !g_enabled.load() || !width || !height || !menu_art::Ready(0))
        {
            g_hitX1 = 0.0f;
            return;
        }
        float progress = g_progress.load();
        // Shown from the start (fading in) for SHOWN_MS after the start or the last input, then faded out.
        ULONGLONG since = g_kindSince.load(), last = std::max(since, g_lastInput.load());
        float alpha = std::clamp(float(now - since) / FADE_IN_MS, 0.0f, 1.0f);
        if (progress <= 0.0f && now - last > SHOWN_MS)
            alpha *= std::clamp(1.0f - float(now - last - SHOWN_MS) / FADE_OUT_MS, 0.0f, 1.0f);
        if (alpha <= 0.0f)
        {
            g_hitX1 = 0.0f;
            return;
        }
        static settings::Values values;
        static uint64_t frames = 0;
        if (frames++ % 30 == 0)
            values = settings::Get();
        Look look;
        look.kind = int(kind);
        look.portuguese = values.language.rfind("pt", 0) == 0;
        look.device = input::pc::PromptDevice();
        look.style = settings::prompts::Style();
        look.progress = int(std::lround(progress * 64.0f));
        look.width = int(width), look.height = int(height);
        if (!(look == drawnLook) || drawn.pixels.empty())
        {
            Rasterize(look, drawn);
            drawnLook = look;
            g_hitX0 = float(drawn.x) / width, g_hitY0 = float(drawn.y) / height;
            g_hitX1 = float(drawn.x + drawn.w) / width, g_hitY1 = float(drawn.y + drawn.h) / height;
        }
        const uint32_t* pixels = drawn.pixels.data();
        uint64_t version = drawn.version;
        if (alpha < 1.0f)
        {
            uint32_t k = uint32_t(std::lround(alpha * 256.0f));
            drawn.faded.resize(drawn.pixels.size());
            for (size_t i = 0; i < drawn.pixels.size(); i++)
                drawn.faded[i] = (drawn.pixels[i] & 0x00FFFFFFu) | (((drawn.pixels[i] >> 24) * k >> 8) << 24);
            pixels = drawn.faded.data();
            version = (1ull << 63) | ++fadedVersion;
        }
        gpu::overlay::DrawBitmap(list, target, width, height, pixels, drawn.w, drawn.h, drawn.x, drawn.y, version,
                                 gpu::overlay::BITMAP_SKIP_PROMPT);
    }
}
