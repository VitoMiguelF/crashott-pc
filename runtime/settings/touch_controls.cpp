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
        // The intro videos play before the frontend runs, and nothing is played in a cutscene: a
        // finger held anywhere skips them (skip_cinematics.cpp), no controls drawn.
        return input::pc::InMenus() || cutscenes::VideoPlaying() || cutscenes::SkipPossible() ? Mode::Mouse : Mode::Game;
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

    // The stick's centre stays where the finger landed; past the full deflection the knob stays on
    // the edge (Draw) and the stick at full.
    void MoveStickLocked(float x, float y)
    {
        g_stick.x = x, g_stick.y = y;
    }

    // The finger's deflection (0..1) as the stick: a small dead zone of its own, then straight to
    // where the game starts to move Crash (it ignores about the first 40 % of a pad's stick, so a
    // finger had to go half way before anything happened), then linear to full.
    constexpr float TOUCH_DEADZONE = 0.08f, GAME_DEADZONE = 0.4f;
    float StickCurve(float deflection)
    {
        if (deflection <= TOUCH_DEADZONE)
            return 0.0f;
        return GAME_DEADZONE + (1.0f - GAME_DEADZONE) * std::min((deflection - TOUCH_DEADZONE) / (1.0f - TOUCH_DEADZONE), 1.0f);
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

    // ---- the action buttons: Crash's shadow doing the action, on fire (the frontend's shadow
    // play behind the menus) ----
    //
    // Each picture is drawn here from shapes (no game file): ellipses, round-capped strokes and
    // polygons on a 100 x 100 square whose middle is the button's (the plate a circle of radius
    // 46). The shapes are joined by their distance field (the nearest one), so the shadow is one
    // piece with one transparency: no darker overlaps where a limb crosses the body.
    struct Point
    {
        float x, y;
    };

    struct Silhouette
    {
        struct Ellipse
        {
            float x, y, rx, ry;
        };
        struct Stroke
        {
            Point a, b;
            float r;
        };
        struct Polygon
        {
            std::vector<Point> points;
            float r;  // outline half width
        };
        std::vector<Ellipse> ellipses;
        std::vector<Stroke> strokes;
        std::vector<Polygon> polygons;

        void E(float x, float y, float rx, float ry) { ellipses.push_back({ x, y, rx, ry }); }
        void C(float x, float y, float r) { ellipses.push_back({ x, y, r, r }); }
        // A polyline `width` wide with round ends and joints.
        void L(std::initializer_list<Point> points, float width)
        {
            for (auto it = points.begin(); it + 1 != points.end(); ++it)
                strokes.push_back({ it[0], it[1], width * 0.5f });
        }
        // A quadratic curve `width` wide.
        void Q(Point a, Point c, Point b, float width)
        {
            Point previous = a;
            for (int i = 1; i <= 12; i++)
            {
                float t = i / 12.0f, u = 1.0f - t;
                Point next{ u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y };
                strokes.push_back({ previous, next, width * 0.5f });
                previous = next;
            }
        }
        void P(std::vector<Point> points, float outline = 0.0f) { polygons.push_back({ std::move(points), outline }); }

        // Crash's head facing right, `(x, y)` the middle of the skull: the skull, the long snout
        // with its nose, the tuft of three spikes and an ear.
        void Head(float x, float y)
        {
            E(x - 1, y - 1, 10, 10.5f);
            E(x + 8, y + 4, 8.5f, 6);
            C(x + 16, y + 1, 2.6f);
            P({ { x - 5, y - 8 }, { x - 13, y - 19 }, { x, y - 11 } });
            P({ { x - 1, y - 10 }, { x, y - 23 }, { x + 6, y - 9 } });
            P({ { x + 4, y - 9 }, { x + 12, y - 18 }, { x + 8, y - 5 } });
            P({ { x - 9, y - 4 }, { x - 15, y - 8 }, { x - 9, y + 1 } });
        }

        // Signed distance to the shadow (negative inside), in the square's units. The ellipses'
        // is the usual approximation (exact on circles), enough for the edge's antialiasing.
        float Distance(Point p) const
        {
            float d = 1e9f;
            for (const Ellipse& e : ellipses)
            {
                float k = std::hypot((p.x - e.x) / e.rx, (p.y - e.y) / e.ry);
                d = std::min(d, (k - 1.0f) * std::min(e.rx, e.ry));
            }
            for (const Stroke& s : strokes)
            {
                float vx = s.b.x - s.a.x, vy = s.b.y - s.a.y, wx = p.x - s.a.x, wy = p.y - s.a.y;
                float t = std::clamp((wx * vx + wy * vy) / std::max(vx * vx + vy * vy, 1e-6f), 0.0f, 1.0f);
                d = std::min(d, std::hypot(wx - vx * t, wy - vy * t) - s.r);
            }
            for (const Polygon& polygon : polygons)
            {
                const auto& v = polygon.points;
                float edge = 1e9f;
                bool inside = false;
                for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++)
                {
                    float vx = v[i].x - v[j].x, vy = v[i].y - v[j].y, wx = p.x - v[j].x, wy = p.y - v[j].y;
                    float t = std::clamp((wx * vx + wy * vy) / std::max(vx * vx + vy * vy, 1e-6f), 0.0f, 1.0f);
                    edge = std::min(edge, std::hypot(wx - vx * t, wy - vy * t));
                    if ((v[i].y > p.y) != (v[j].y > p.y) && p.x < v[j].x + vx * (p.y - v[j].y) / vy)
                        inside = !inside;
                }
                d = std::min(d, (inside ? -edge : edge) - polygon.r);
            }
            return d;
        }
    };

    // The pictures (the sketch the user picked, "v3"): A jumps high over his dust, X punches, Y
    // a flying kick, B reaches for a mutant (B interacts and jacks the mutants), RT guards behind
    // a shield a hit bounces off, SPIN spins.
    Silhouette MakeSilhouette(int button)
    {
        Silhouette s;
        switch (button)
        {
        case BTN_A:
            s.Head(50, 21);
            s.L({ { 50, 29 }, { 49, 45 } }, 9);
            s.L({ { 51, 32 }, { 61, 25 }, { 65, 15 } }, 5);
            s.L({ { 49, 32 }, { 39, 26 }, { 35, 16 } }, 5);
            s.L({ { 49, 45 }, { 57, 52 }, { 52, 60 } }, 5);
            s.L({ { 49, 45 }, { 41, 52 }, { 45, 60 } }, 5);
            s.C(66, 12, 5), s.C(34, 13, 5);
            s.E(56, 61, 7, 3.4f), s.E(47, 62, 7, 3.4f);
            s.L({ { 42, 69 }, { 42, 75 } }, 3), s.L({ { 50, 69 }, { 50, 79 } }, 3), s.L({ { 58, 69 }, { 58, 75 } }, 3);
            s.E(50, 88, 20, 4), s.C(31, 85, 4), s.C(69, 85, 4);
            break;
        case BTN_X:
            s.Head(38, 32);
            s.L({ { 42, 38 }, { 38, 58 } }, 9);
            s.L({ { 44, 42 }, { 58, 42 }, { 71, 40 } }, 5);
            s.L({ { 40, 42 }, { 30, 48 }, { 30, 56 } }, 5);
            s.L({ { 38, 58 }, { 50, 68 }, { 54, 79 } }, 5);
            s.L({ { 38, 58 }, { 28, 68 }, { 21, 76 } }, 5);
            s.C(76, 40, 6.5f), s.C(30, 59, 4.6f);
            s.E(59, 81, 7.5f, 3.6f), s.E(19, 79, 7.5f, 3.6f);
            s.L({ { 86, 31 }, { 91, 27 } }, 3), s.L({ { 87, 40 }, { 94, 40 } }, 3), s.L({ { 86, 49 }, { 91, 53 } }, 3);
            break;
        case BTN_Y:
            s.Head(27, 34);
            s.L({ { 31, 40 }, { 45, 53 } }, 9);
            s.L({ { 45, 53 }, { 60, 51 }, { 73, 48 } }, 5);
            s.L({ { 45, 53 }, { 43, 65 }, { 36, 70 } }, 5);
            s.L({ { 33, 43 }, { 23, 51 } }, 5);
            s.L({ { 33, 41 }, { 41, 33 }, { 47, 27 } }, 5);
            s.E(79, 47, 4, 7.5f), s.E(33, 72, 7, 3.5f);
            s.C(21, 53, 4.6f), s.C(49, 24, 4.6f);
            s.P({ { 88, 38 }, { 90, 45 }, { 96, 46 }, { 91, 50 }, { 93, 57 }, { 88, 53 }, { 83, 57 }, { 85, 50 }, { 80, 46 }, { 86, 45 } });
            break;
        case BTN_B:
            s.Head(26, 36);
            s.L({ { 28, 44 }, { 28, 64 } }, 9);
            s.L({ { 31, 47 }, { 44, 45 }, { 55, 41 } }, 5);
            s.L({ { 26, 48 }, { 19, 56 } }, 5);
            s.L({ { 28, 64 }, { 36, 74 }, { 38, 82 } }, 5);
            s.L({ { 28, 64 }, { 21, 74 }, { 19, 82 } }, 5);
            s.C(18, 58, 4.6f), s.E(42, 83, 7.5f, 3.6f), s.E(22, 84, 7.5f, 3.6f), s.C(58, 40, 4.4f);
            s.L({ { 60, 36 }, { 64, 33 } }, 2.6f), s.L({ { 61, 40 }, { 66, 39 } }, 2.6f), s.L({ { 60, 44 }, { 64, 46 } }, 2.6f);
            // The mutant: a horned head on a body.
            s.E(80, 52, 12, 14), s.E(80, 74, 14, 8);
            s.P({ { 70, 44 }, { 64, 26 }, { 76, 40 } }), s.P({ { 88, 40 }, { 96, 24 }, { 92, 46 } });
            break;
        case BTN_RT:
            s.Head(30, 40);
            s.L({ { 32, 46 }, { 30, 64 } }, 9);
            s.L({ { 34, 50 }, { 44, 44 }, { 50, 38 } }, 5);
            s.L({ { 32, 54 }, { 44, 54 }, { 50, 48 } }, 5);
            s.L({ { 30, 64 }, { 40, 70 }, { 44, 82 } }, 5);
            s.L({ { 30, 64 }, { 20, 72 }, { 16, 82 } }, 5);
            s.E(49, 84, 7.5f, 3.6f), s.E(14, 84, 7.5f, 3.6f);
            {
                // The shield: straight top and side, curved down to its point.
                std::vector<Point> shield{ { 56, 26 }, { 74, 32 }, { 74, 54 } };
                for (int i = 1; i <= 8; i++)
                {
                    float t = i / 8.0f, u = 1 - t;
                    shield.push_back({ u * u * 74 + 2 * u * t * 74 + t * t * 56, u * u * 54 + 2 * u * t * 70 + t * t * 80 });
                }
                for (int i = 1; i <= 8; i++)
                {
                    float t = i / 8.0f, u = 1 - t;
                    shield.push_back({ u * u * 56 + 2 * u * t * 50 + t * t * 50, u * u * 80 + 2 * u * t * 70 + t * t * 56 });
                }
                s.P(std::move(shield), 1.5f);
            }
            s.L({ { 84, 34 }, { 92, 30 } }, 3), s.L({ { 86, 46 }, { 95, 46 } }, 3), s.L({ { 84, 58 }, { 92, 62 } }, 3);
            break;
        case BTN_SPIN:
            s.Head(48, 32);
            s.L({ { 50, 38 }, { 50, 58 } }, 9);
            s.L({ { 50, 43 }, { 30, 45 } }, 5), s.L({ { 50, 43 }, { 70, 41 } }, 5);
            s.L({ { 50, 58 }, { 44, 72 } }, 5), s.L({ { 50, 58 }, { 56, 72 } }, 5);
            s.C(27, 45, 4.6f), s.C(73, 41, 4.6f), s.E(42, 74, 7, 3.4f), s.E(59, 74, 7, 3.4f);
            s.Q({ 14, 54 }, { 50, 70 }, { 86, 52 }, 3), s.Q({ 18, 34 }, { 50, 22 }, { 82, 32 }, 3), s.Q({ 28, 86 }, { 50, 94 }, { 72, 86 }, 3);
            break;
        default:
            break;
        }
        return s;
    }

    // The fire plate (see-through, brighter while held), its light rim and the shadow over it, as
    // a premultiplied `size` x `size` image of a button of radius `radius` pixels.
    menu_art::Image ActionButton(int button, int size, float radius, bool down, float edgeWidth)
    {
        static Silhouette shapes[BUTTON_COUNT];
        static bool made = false;
        if (!made)
        {
            for (int b = 0; b < BUTTON_COUNT; b++)
                shapes[b] = MakeSilhouette(b);
            made = true;
        }
        const Silhouette& shadow = shapes[button];
        // The picture a little inside the rim: 90 % of the plate.
        const float units = radius / 46.0f * 0.9f;  // pixels per unit of the square
        const Color fire = down ? Color{ 255, 178, 60, 190 } : Color{ 239, 159, 39, 97 };
        const Color rim = down ? Color{ 255, 238, 200, 230 } : Color{ 250, 238, 218, 140 };
        constexpr float SHADOW_R = 44 / 255.0f, SHADOW_G = 26 / 255.0f, SHADOW_B = 4 / 255.0f, SHADOW_A = 0.8f;
        menu_art::Image image;
        image.w = image.h = size;
        image.px.assign(size_t(size) * size, 0);
        const float c = size * 0.5f;
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                float px = x + 0.5f - c, py = y + 0.5f - c;
                float d = std::hypot(px, py) - radius;
                float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cover <= 0.0f)
                    continue;
                float t = std::clamp(-d - edgeWidth + 0.5f, 0.0f, 1.0f);  // 0 on the rim, 1 inside
                float a = (rim.a * (1 - t) + fire.a * t) / 255.0f * cover;
                float r = (rim.r * (1 - t) + fire.r * t) / 255.0f * a, g = (rim.g * (1 - t) + fire.g * t) / 255.0f * a,
                      b = (rim.b * (1 - t) + fire.b * t) / 255.0f * a;
                float sd = shadow.Distance({ 50.0f + px / units, 50.0f + py / units }) * units;
                float s = std::clamp(0.5f - sd, 0.0f, 1.0f) * std::clamp(-d - edgeWidth + 0.5f, 0.0f, 1.0f) * SHADOW_A;
                r = SHADOW_R * s + r * (1 - s), g = SHADOW_G * s + g * (1 - s), b = SHADOW_B * s + b * (1 - s), a = s + a * (1 - s);
                auto byte = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                image.px[size_t(y) * size + x] = byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
            }
        }
        return image;
    }

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
            if (CLUSTER >> b & 1)
            {
                canvas.Blit(ActionButton(b, size, spot.r, down, edgeWidth), bx, by);
                continue;
            }
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
                MoveStickLocked(px, py);
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
            MoveStickLocked(px, py);
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
            float curved = StickCurve(length);
            if (curved > 0.0f)
                x *= curved / length, y *= curved / length;
            else
                x = y = 0.0f;
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
