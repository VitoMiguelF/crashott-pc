// On-screen touch controls (touch_controls.h). The fingers come from the window thread, the game
// reads the result as controller 0 (Apply) and the presenter draws them (Draw), so the state is
// shared under g_mutex. The layout is in back buffer pixels, sized by its height (s = height / 720).

#include "touch_controls.h"
#include "menu_art.h"
#include "settings_menu.h"
#include "skip_cinematics.h"
#include "../dev/dev_console.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../kernel/pc_input.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    using settings::menu_art::Color;
    namespace menu_art = settings::menu_art;

    enum Button { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_RT, BTN_SPIN, BTN_START, BTN_BACK, BUTTON_COUNT };
    constexpr char32_t ICONS[BUTTON_COUNT] = { menu_art::ICON_A, menu_art::ICON_B, menu_art::ICON_X, menu_art::ICON_Y, menu_art::ICON_RT,
                                               menu_art::ICON_ROTATE, menu_art::ICON_START, menu_art::ICON_BACK };
    constexpr uint16_t BITS[BUTTON_COUNT] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, 0, 0,
                                              XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK };
    constexpr const char* NAMES[BUTTON_COUNT] = { "A", "B", "X", "Y", "RT", "SPIN", "START", "BACK" };
    constexpr uint32_t CLUSTER = 1u << BTN_A | 1u << BTN_B | 1u << BTN_X | 1u << BTN_Y | 1u << BTN_RT | 1u << BTN_SPIN;
    constexpr uint32_t TOP = 1u << BTN_START | 1u << BTN_BACK;

    // Where the touch goes: the game's controls (gameplay), or the mouse (the menus, whose prompts
    // are clickable: prompt_icons.h ButtonAt; the settings screen; the console).
    enum class Mode { Mouse, Game };

    struct Spot
    {
        float x = 0, y = 0, r = 0;
    };

    struct Layout
    {
        float w = 0, h = 0, s = 0;
        Spot buttons[BUTTON_COUNT];
        Spot stickHome;         // the stick's centre while no finger holds it
        float stickRange = 0;   // finger distance for full deflection
        float baseR = 0, knobR = 0;
        float stickZone = 0;    // a finger left of this is the stick's
    };

    Layout MakeLayout(float w, float h)
    {
        Layout l;
        l.w = w, l.h = h, l.s = h / 720.0f;
        const float s = l.s;
        // The pad's diamond at the bottom right, RT and the spin to its left.
        float cx = w - 200.0f * s, cy = h - 190.0f * s, d = 92.0f * s, r = 46.0f * s;
        l.buttons[BTN_A] = { cx, cy + d, r };
        l.buttons[BTN_B] = { cx + d, cy, r };
        l.buttons[BTN_X] = { cx - d, cy, r };
        l.buttons[BTN_Y] = { cx, cy - d, r };
        l.buttons[BTN_RT] = { cx - 2.1f * d, cy + 0.95f * d, 42.0f * s };
        l.buttons[BTN_SPIN] = { cx - 2.1f * d, cy - 0.35f * d, 42.0f * s };
        l.buttons[BTN_START] = { w - 70.0f * s, 64.0f * s, 34.0f * s };
        l.buttons[BTN_BACK] = { w - 160.0f * s, 64.0f * s, 34.0f * s };
        l.stickHome = { 200.0f * s, h - 190.0f * s, 0 };
        l.stickRange = 100.0f * s;
        l.baseR = 95.0f * s, l.knobR = 45.0f * s;
        l.stickZone = w * 0.45f;
        return l;
    }

    enum class Role { None, Stick, Button, Camera };

    struct Finger
    {
        bool used = false;
        uint64_t id = 0;
        Role role = Role::None;
        int button = 0;
        float x = 0, y = 0;  // pixels
    };

    std::mutex g_mutex;
    Finger g_fingers[10];
    struct
    {
        bool active = false;
        float ox = 0, oy = 0, x = 0, y = 0;  // centre and finger, pixels
    } g_stick;
    float g_cameraX = 0, g_cameraY = 0;     // finger motion since the last game read, pixels
    bool g_hidden = false;                  // a pad or the keyboard took over
    std::atomic<uint32_t> g_layoutW{ 0 }, g_layoutH{ 0 };  // the back buffer (Draw)

    // The left stick going round while SPIN is held (pc_input.cpp's spin attack: a turn every 9
    // game reads, X tapped once the first turn is done, then 2 reads down, 2 up).
    struct
    {
        bool active = false;
        float angle = 0;
        int reads = 0;
    } g_spin;
    constexpr float SPIN_STEP = 6.2831853f / 9.0f;
    constexpr int SPIN_FIRST_PRESS = 9, SPIN_PERIOD = 4;

    Layout CurrentLayout()
    {
        uint32_t w = g_layoutW.load(), h = g_layoutH.load();
        return w && h ? MakeLayout(float(w), float(h)) : MakeLayout(1280.0f, 720.0f);
    }

    Mode CurrentMode()
    {
        if (!touch_controls::Enabled() || settings::menu::IsOpen() || dev::console::IsOpen())
            return Mode::Mouse;
        // The intro videos play before the frontend runs: their skip prompt takes the touch.
        return input::pc::InMenus() || cutscenes::VideoPlaying() ? Mode::Mouse : Mode::Game;
    }

    // The button of `set` under (x, y), within 1.3 x its radius (the nearest one), or -1.
    int HitButton(const Layout& l, uint32_t set, float x, float y)
    {
        int best = -1;
        float bestD = 0;
        for (int b = 0; b < BUTTON_COUNT; b++)
        {
            if (!(set >> b & 1))
                continue;
            const Spot& spot = l.buttons[b];
            float d = std::hypot(x - spot.x, y - spot.y) / spot.r;
            if (d <= 1.3f && (best < 0 || d < bestD))
                best = b, bestD = d;
        }
        return best;
    }

    uint32_t PressedLocked()
    {
        uint32_t mask = 0;
        for (const Finger& f : g_fingers)
        {
            if (f.used && f.role == Role::Button)
                mask |= 1u << f.button;
        }
        return mask;
    }

    bool AnyFingerLocked()
    {
        return std::any_of(std::begin(g_fingers), std::end(g_fingers), [](const Finger& f) { return f.used && f.role != Role::None; });
    }

    // The stick's centre follows a finger that goes past the full deflection.
    void MoveStickLocked(const Layout& l, float x, float y)
    {
        g_stick.x = x, g_stick.y = y;
        float dx = x - g_stick.ox, dy = y - g_stick.oy, d = std::hypot(dx, dy);
        if (d > l.stickRange)
        {
            g_stick.ox = x - dx / d * l.stickRange;
            g_stick.oy = y - dy / d * l.stickRange;
        }
    }

    int16_t Axis(float value)
    {
        return int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    }

    // ---- drawing ----

    // Premultiplied antialiased disc of radius `radius` in a `size` x `size` image, with an edge.
    menu_art::Image Disc(int size, float radius, Color fill, Color edge, float edgeWidth)
    {
        menu_art::Image image;
        image.w = image.h = size;
        image.px.assign(size_t(size) * size, 0);
        const float c = size * 0.5f;
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                float d = std::hypot(x + 0.5f - c, y + 0.5f - c) - radius;  // negative inside
                float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cover <= 0.0f)
                    continue;
                float t = std::clamp(-d - edgeWidth + 0.5f, 0.0f, 1.0f);  // 0 on the edge, 1 inside
                float a = (edge.a * (1.0f - t) + fill.a * t) / 255.0f * cover;
                float r = edge.r * (1.0f - t) + fill.r * t, g = edge.g * (1.0f - t) + fill.g * t, b = edge.b * (1.0f - t) + fill.b * t;
                auto p = [&](float v) { return uint32_t(std::lround(v * a)); };
                image.px[size_t(y) * size + x] = p(r) | p(g) << 8 | p(b) << 16 | uint32_t(std::lround(a * 255.0f)) << 24;
            }
        }
        return image;
    }

    constexpr Color PLATE{ 14, 8, 0, 105 }, EDGE{ 236, 222, 190, 120 };
    constexpr Color PLATE_DOWN{ 232, 160, 0, 150 }, EDGE_DOWN{ 255, 235, 190, 230 };

    struct Drawn
    {
        std::vector<uint32_t> pixels;
        int w = 0, h = 0, x = 0, y = 0;
        uint64_t version = 0;
        uint64_t look = ~0ull;  // what it was drawn from
    };

    // The buttons of `set` into one bitmap (their bounding box).
    void RasterizeButtons(const Layout& l, uint32_t set, uint32_t pressed, Drawn& out)
    {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int b = 0; b < BUTTON_COUNT; b++)
        {
            if (set >> b & 1)
            {
                const Spot& spot = l.buttons[b];
                x0 = std::min(x0, spot.x - spot.r), y0 = std::min(y0, spot.y - spot.r);
                x1 = std::max(x1, spot.x + spot.r), y1 = std::max(y1, spot.y + spot.r);
            }
        }
        int left = int(std::floor(x0)) - 2, top = int(std::floor(y0)) - 2;
        int w = int(std::ceil(x1)) + 2 - left, h = int(std::ceil(y1)) + 2 - top;
        menu_art::Canvas canvas;
        canvas.Reset(w, h);
        float edgeWidth = std::max(1.5f, 2.5f * l.s);
        for (int b = 0; b < BUTTON_COUNT; b++)
        {
            if (!(set >> b & 1))
                continue;
            const Spot& spot = l.buttons[b];
            bool down = pressed >> b & 1;
            int size = int(std::ceil(spot.r * 2.0f)) + 2;
            int bx = int(std::lround(spot.x - size * 0.5f)) - left, by = int(std::lround(spot.y - size * 0.5f)) - top;
            canvas.Blit(Disc(size, spot.r, down ? PLATE_DOWN : PLATE, down ? EDGE_DOWN : EDGE, edgeWidth), bx, by);
            float iconH = spot.r * 1.15f, iconW = menu_art::IconWidth(ICONS[b], iconH);
            menu_art::DrawIcon(canvas, ICONS[b], spot.x - left - iconW * 0.5f, spot.y - top - iconH * 0.5f, iconH, down ? 1.0f : 0.7f);
        }
        canvas.Export(out.pixels);
        out.w = w, out.h = h, out.x = left, out.y = top;
        out.version++;
    }

    // The stick: its base and the knob `kx`, `ky` pixels from the centre.
    void RasterizeStick(const Layout& l, bool active, int kx, int ky, Drawn& out)
    {
        int size = int(std::ceil((l.stickRange + l.knobR) * 2.0f)) + 4;
        menu_art::Canvas canvas;
        canvas.Reset(size, size);
        float edgeWidth = std::max(1.5f, 2.5f * l.s);
        int baseSize = int(std::ceil(l.baseR * 2.0f)) + 2;
        canvas.Blit(Disc(baseSize, l.baseR, Color{ 14, 8, 0, uint8_t(active ? 90 : 60) }, Color{ 236, 222, 190, uint8_t(active ? 130 : 90) },
                         edgeWidth),
                    (size - baseSize) / 2, (size - baseSize) / 2);
        int knobSize = int(std::ceil(l.knobR * 2.0f)) + 2;
        canvas.Blit(Disc(knobSize, l.knobR, active ? Color{ 236, 222, 190, 150 } : Color{ 236, 222, 190, 80 },
                         active ? EDGE_DOWN : EDGE, edgeWidth),
                    (size - knobSize) / 2 + kx, (size - knobSize) / 2 + ky);
        canvas.Export(out.pixels);
        out.w = out.h = size;
        out.version++;
    }
}

namespace touch_controls
{
    bool Enabled()
    {
        static bool on = [] {
#if defined(__ANDROID__)
            bool on = true;
#else
            bool on = false;
#endif
            if (const char* value = getenv("CRASH_TOUCH_CONTROLS"); value && *value)
                on = strcmp(value, "0") != 0;
            return on;
        }();
        return on;
    }

    bool OnFinger(uint64_t finger, Phase phase, float x, float y)
    {
        if (!Enabled())
            return false;
        Layout l = CurrentLayout();
        float px = x * l.w, py = y * l.h;
        std::lock_guard lock(g_mutex);
        Finger* f = nullptr;
        for (Finger& candidate : g_fingers)
        {
            if (candidate.used && candidate.id == finger)
                f = &candidate;
        }

        if (phase == Phase::Down)
        {
            if (g_hidden)
            {
                g_hidden = false;
                Log("[touch] controls shown again");
            }
            if (CurrentMode() != Mode::Game)
                return false;
            if (!f)
            {
                for (Finger& candidate : g_fingers)
                {
                    if (!candidate.used)
                    {
                        f = &candidate;
                        break;
                    }
                }
                if (!f)
                    return true;  // more fingers than slots
            }
            *f = { true, finger, Role::None, 0, px, py };
            int button = HitButton(l, CLUSTER | TOP, px, py);
            if (button >= 0)
            {
                f->role = Role::Button, f->button = button;
                Log("[touch] %s down", NAMES[button]);
                return true;
            }
            bool stickHeld = std::any_of(std::begin(g_fingers), std::end(g_fingers),
                                         [](const Finger& o) { return o.used && o.role == Role::Stick; });
            if (px < l.stickZone && !stickHeld && py > l.h * 0.2f)
            {
                f->role = Role::Stick;
                // The centre where the finger landed, kept far enough from the edges for the base.
                g_stick = { true, std::clamp(px, l.baseR, l.stickZone), std::clamp(py, l.baseR, l.h - l.baseR), px, py };
                MoveStickLocked(l, px, py);
            }
            else if (px >= l.stickZone)
                f->role = Role::Camera;
            return true;
        }

        if (!f)
            return false;
        float dx = px - f->x, dy = py - f->y;
        f->x = px, f->y = py;
        switch (f->role)
        {
        case Role::Stick:
            MoveStickLocked(l, px, py);
            break;
        case Role::Camera:
            g_cameraX += dx, g_cameraY += dy;
            break;
        case Role::Button:
            if (phase == Phase::Move)
            {
                // Sliding onto another button moves the press there.
                int button = HitButton(l, CLUSTER | TOP, px, py);
                if (button >= 0 && button != f->button)
                {
                    f->button = button;
                    Log("[touch] slid to %s", NAMES[button]);
                }
            }
            break;
        default:
            break;
        }
        if (phase == Phase::Up)
        {
            if (f->role == Role::Stick)
                g_stick.active = false;
            f->used = false;
        }
        return true;
    }

    void Apply(XINPUT_GAMEPAD& pad, bool poll)
    {
        if (!Enabled())
            return;
        std::lock_guard lock(g_mutex);
        bool touching = AnyFingerLocked();
        // A pad in use (the pad alone reaches here) hides the controls.
        bool padUsed = pad.wButtons || pad.bLeftTrigger > 64 || pad.bRightTrigger > 64 || std::abs(int(pad.sThumbLX)) > 16000 ||
                       std::abs(int(pad.sThumbLY)) > 16000 || std::abs(int(pad.sThumbRX)) > 16000 || std::abs(int(pad.sThumbRY)) > 16000;
        if (padUsed && !touching && !g_hidden)
        {
            g_hidden = true;
            Log("[touch] a controller is in use: controls hidden until the next touch");
        }
        if (!touching)
        {
            g_spin.active = false;
            g_cameraX = g_cameraY = 0;
            return;
        }

        Layout l = CurrentLayout();
        uint32_t pressed = PressedLocked();
        uint16_t buttons = 0;
        for (int b = 0; b < BUTTON_COUNT; b++)
        {
            if (pressed >> b & 1)
                buttons |= BITS[b];
        }
        if (pressed >> BTN_RT & 1)
            pad.bRightTrigger = 255;

        float x = 0, y = 0;
        if (g_stick.active)
        {
            x = (g_stick.x - g_stick.ox) / l.stickRange, y = (g_stick.oy - g_stick.y) / l.stickRange;
            float length = std::hypot(x, y);
            if (length > 1.0f)
                x /= length, y /= length;
        }
        if (pressed >> BTN_SPIN & 1)
        {
            if (!g_spin.active)
                g_spin = { true, x != 0.0f || y != 0.0f ? std::atan2(y, x) : 1.5707963f, 0 };
            else if (poll)
                g_spin.angle += SPIN_STEP, g_spin.reads++;
            // Full deflection: the game ignores the stick going round below 60 %.
            x = std::cos(g_spin.angle), y = std::sin(g_spin.angle);
            if (g_spin.reads >= SPIN_FIRST_PRESS && (g_spin.reads - SPIN_FIRST_PRESS) % SPIN_PERIOD < 2)
                buttons |= XINPUT_GAMEPAD_X;
        }
        else
            g_spin.active = false;
        if (x != 0.0f || y != 0.0f)
            pad.sThumbLX = Axis(x), pad.sThumbLY = Axis(y);

        // The camera: ~12 px (at 720p) of finger motion per game read is full deflection.
        static float s_cameraX = 0, s_cameraY = 0;
        if (poll)
        {
            float k = 1.0f / (12.0f * l.s);
            s_cameraX = g_cameraX * k, s_cameraY = -g_cameraY * k;
            g_cameraX = g_cameraY = 0;
        }
        if (s_cameraX != 0.0f || s_cameraY != 0.0f)
            pad.sThumbRX = Axis(s_cameraX), pad.sThumbRY = Axis(s_cameraY);

        pad.wButtons |= buttons;
        if (poll)
            input::pc::NoteDevice(input::pc::Device::Xbox);  // the prompts show the pad's buttons
    }

    void Draw(uint32_t width, uint32_t height)
    {
        if (!Enabled() || !width || !height)
            return;
        g_layoutW = width, g_layoutH = height;
        Mode mode = CurrentMode();
        uint32_t pressed;
        bool stickActive;
        float ox, oy, sx, sy;
        {
            std::lock_guard lock(g_mutex);
            if (g_hidden)
                mode = Mode::Mouse;
            pressed = PressedLocked();
            stickActive = g_stick.active;
            ox = g_stick.ox, oy = g_stick.oy, sx = g_stick.x, sy = g_stick.y;
        }
        if (mode != Mode::Game || !menu_art::Ready(0))
            return;
        Layout l = MakeLayout(float(width), float(height));
        uint64_t size = uint64_t(width) << 32 | height;

        static Drawn cluster, top, stick;
        auto draw = [&](Drawn& d, uint32_t set, gpu::overlay::BitmapChannel channel) {
            uint64_t look = size * 31 + (uint64_t(set) << 16 | (pressed & set));
            if (d.look != look || d.pixels.empty())
            {
                RasterizeButtons(l, set, pressed, d);
                d.look = look;
            }
            gpu::overlay::DrawBitmap(width, height, d.pixels.data(), d.w, d.h, d.x, d.y, d.version, channel);
        };
        draw(cluster, CLUSTER, gpu::overlay::BITMAP_TOUCH_BUTTONS);
        draw(top, TOP, gpu::overlay::BITMAP_TOUCH_TOP);

        // The stick: at its home while no finger holds it.
        int kx = 0, ky = 0;
        if (stickActive)
        {
            float dx = sx - ox, dy = sy - oy, d = std::hypot(dx, dy);
            if (d > l.stickRange)
                dx *= l.stickRange / d, dy *= l.stickRange / d;
            kx = int(std::lround(dx)), ky = int(std::lround(dy));
        }
        else
            ox = l.stickHome.x, oy = l.stickHome.y;
        uint64_t look = size * 31 + (uint64_t(stickActive) << 40 | uint64_t(uint16_t(kx)) << 16 | uint16_t(ky));
        if (stick.look != look || stick.pixels.empty())
        {
            RasterizeStick(l, stickActive, kx, ky, stick);
            stick.look = look;
        }
        int x = int(std::lround(ox)) - stick.w / 2, y = int(std::lround(oy)) - stick.h / 2;
        gpu::overlay::DrawBitmap(width, height, stick.pixels.data(), stick.w, stick.h, x, y, stick.version, gpu::overlay::BITMAP_TOUCH_STICK);
    }
}
