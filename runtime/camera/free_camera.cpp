// Free third-person camera (experimental): an orbit camera around the player that replaces the
// game's scripted / volume cameras during gameplay. See free_camera.h for the idea; the guest
// structures it relies on (all found by reading the recompiled code, runtime/NOTES.md "Câmera
// livre"):
//
//   managers      sub_821FB4F0() = 0x824A7A24, manager i at +28 + 4*i: 4 = CCameraManager,
//                 5 = CCollisionManager, 12 = CRenderManager, 18 = CCinematicsManager.
//   view camera   CRenderManager +356: pure3d::VectorCamera. +16 FOV, +32/+36 near/far,
//                 +80 world->camera and +144 camera->world matrices (rows: right, up, forward,
//                 position), +208 "matrices valid" byte, +216 position, +228 direction, +240 up.
//                 The setters (sub_822F2038 / 822F1FE8 / 822F2010) write the vector and clear +208;
//                 the matrices are rebuilt from the vectors on the next read (VectorCamera::vf26).
//   controller    CCameraBehaviourController::vf08 (sub_820E0498), phase r6 == 3: runs the camera
//                 behaviours (volumes, script cameras, transitions), adds the shake and copies its
//                 matrix (*(this+168)) into the VectorCamera. It is not called while a cinematic
//                 (CCinematicCameraAction) drives the camera.
//   player        CControllerGroundBehaviour::vf08 (sub_820E96F0): this+28 = the actor, actor+44 =
//                 its world matrix (position at +48; +32 is not a reliable facing), **(this+52) = its input map
//                 (+36 != 0: driven by a controller). The input map's channel c is the float at
//                 hist[idx * 4] with {hist, ?, idx} = input + (c + 4) * 12: 0/1 left stick x/y,
//                 2/3 right stick x/y (up = +1); the button channels move with the control
//                 scheme, so R3 (recenter) comes from controller 0 (input::MenuButtons).
//   walking       sub_821EBC70(input, out, x, y): stick -> world with the VectorCamera's
//                 camera->world forward (flattened); sub_821EBA50 before it keeps the previous
//                 forward while the stick is held across a camera cut (+516 bit 0x80, +468).
//   collision     sub_821C5EA0(collision manager + 4, ray, hit, 1): ray = {from xyz, to xyz,
//                 -1, -1, 0}, returns hit and the distance in hit[0] (the blob shadow's
//                 downward test, sub_8222E608).

#include "free_camera.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/pc_input.h"
#include "../kernel/memory.h"
#include "../settings/settings.h"
#include "../platform/platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_820E0498);  // CCameraBehaviourController::vf08 (update)
extern "C" PPC_FUNC(__imp__sub_820E96F0);  // CControllerGroundBehaviour::vf08 (update)
extern "C" PPC_FUNC(__imp__sub_820E63A0);  // CScriptCameraBehaviour::vf08 (update)
extern "C" PPC_FUNC(__imp__sub_821B6EC8);  // CCinematicCameraAction::vf04 (update)
extern "C" PPC_FUNC(__imp__sub_821EBA50);  // input map: camera-cut direction lock
extern "C" PPC_FUNC(__imp__sub_821EBC70);  // input map: stick -> world (heading, probe)
PPC_EXTERN_FUNC(sub_821C5EA0);             // collision: ray test

namespace
{
    constexpr uint32_t MANAGERS = 0x824A7A24;
    constexpr uint32_t COLLISION_MANAGER = 5;
    constexpr uint32_t RENDER_MANAGER = 12;

    constexpr float PI = 3.14159265f;
    constexpr float DEG = PI / 180.0f;

    // Tuning (distance and speeds are scaled by the settings).
    constexpr float BASE_DISTANCE = 6.0f;       // game units (the game's follow camera is ~7)
    constexpr float TARGET_HEIGHT = 1.3f;       // above the actor's origin (its feet)
    constexpr float MIN_DISTANCE = 1.4f;       // never closer (invisible walls are often right behind the player)
    constexpr float COLLISION_MARGIN = 0.35f;
    constexpr float RADIUS = 0.25f;             // the extra rays around the line of sight
    constexpr float PITCH_MIN = -20.0f * DEG;   // looking up
    constexpr float PITCH_MAX = 70.0f * DEG;    // looking down
    constexpr float RESTING_PITCH = 12.0f * DEG;
    constexpr float YAW_SPEED = 160.0f * DEG;   // per second at full stick, 100% sensitivity
    constexpr float PITCH_SPEED = 100.0f * DEG;
    constexpr float MOUSE_DEG_PER_PIXEL = 0.12f * DEG;
    constexpr float STICK_DEADZONE = 0.15f;
    constexpr float BLEND_SECONDS = 0.35f;      // in and out of the game's camera
    constexpr float RECENTER_RATE = 10.0f;      // 1/s, exponential
    constexpr float DISTANCE_RETURN = 4.0f;     // units/s back out after a collision
    constexpr float TARGET_FOLLOW = 14.0f;      // 1/s, smoothing of the target point
    constexpr float LIFT_MAX = 40.0f * DEG;     // extra pitch when a wall pushes the camera in
    constexpr float LIFT_RATE = 6.0f;           // 1/s
    constexpr float MOUNTED_DISTANCE = 1.6f;    // x distance while riding a titan
    constexpr float MOUNTED_HEIGHT = 1.8f;      // a bit above the rider, so the titan stays in view
    constexpr uint64_t MOUNTED_TICKS = 45;      // ground behaviour stopped this long (longer than a jump)

    struct Vec3
    {
        float x = 0, y = 0, z = 0;
        Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    };
    float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
    Vec3 Normalize(const Vec3& v, const Vec3& fallback = { 0, 0, 1 })
    {
        float l = Length(v);
        return l > 1e-6f ? v * (1.0f / l) : fallback;
    }
    Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

    bool ValidGuest(uint32_t a) { return a >= 0x40000000 && a < 0xE0000000; }

    float LoadF(uint32_t a)
    {
        uint32_t b = LoadBE32(a);
        float f;
        memcpy(&f, &b, 4);
        return f;
    }
    void StoreF(uint32_t a, float f)
    {
        uint32_t b;
        memcpy(&b, &f, 4);
        StoreBE32(a, b);
    }
    Vec3 LoadV(uint32_t a) { return { LoadF(a), LoadF(a + 4), LoadF(a + 8) }; }
    void StoreV(uint32_t a, const Vec3& v)
    {
        StoreF(a, v.x);
        StoreF(a + 4, v.y);
        StoreF(a + 8, v.z);
    }

    uint32_t Manager(uint32_t index) { return LoadBE32(MANAGERS + (index + 7) * 4); }

    // An input map channel (see the header comment), 0 if the map looks wrong.
    float Channel(uint32_t input, uint32_t channel)
    {
        if (!ValidGuest(input))
            return 0;
        uint32_t slot = input + (channel + 4) * 12;
        uint32_t history = LoadBE32(slot), index = LoadBE32(slot + 8);
        if (!ValidGuest(history) || index > 64)
            return 0;
        float v = LoadF(history + index * 16);
        return std::isfinite(v) ? v : 0;
    }

    float WrapAngle(float a)
    {
        while (a > PI) a -= 2 * PI;
        while (a < -PI) a += 2 * PI;
        return a;
    }

    Vec3 DirectionOf(float yaw, float pitch)
    {
        return { std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
    }

    // CRASH_CAMERA_FREE_AFTER=<seconds> (tests): the free camera turns on by itself at that time
    // of the replay's timeline (CRASH_REPLAY), or of the input clock without a replay, whatever
    // the setting says; so a profile recorded with the game's camera plays the same up to there.
    bool ForcedOn()
    {
        static const double after = [] {
            const char* text = getenv("CRASH_CAMERA_FREE_AFTER");
            return text && *text ? atof(text) : -1.0;
        }();
        if (after < 0)
            return false;
        input::RecordStatus status = input::Status();
        double now = status.replaying ? status.replaySeconds : input::ClockMs() / 1000.0;
        return now >= after;
    }

    bool Probe()
    {
        static const bool on = getenv("CRASH_CAMERA_PROBE") != nullptr;
        return on;
    }

    // Shared state. Everything but g_active is touched only by the guest thread that runs the
    // game's update (the hooks below).
    std::atomic<bool> g_active{ false };
    bool g_mouseTaken = false;

    // The mouse turns the free camera itself (kernel/pc_input.h) while it is active; otherwise it
    // goes back to moving the right stick.
    void TakeMouse(bool take)
    {
        if (take == g_mouseTaken)
            return;
        input::pc::TakeMouseLook(take);
        g_mouseTaken = take;
        if (take)
        {
            float dx, dy;
            input::pc::MouseLookDelta(dx, dy);  // drop what moved before
        }
    }

    struct State
    {
        uint32_t controller = 0;     // the CCameraBehaviourController seen last
        uint32_t player = 0;         // actor followed
        uint32_t playerVtable = 0;
        uint32_t playerInput = 0;
        uint64_t playerSeenTick = 0; // tick its input map last drove something (walking, a titan)
        uint64_t groundTick = 0;     // tick its ground behaviour last ran
        uint64_t tick = 0;           // camera controller updates
        uint32_t scriptCameraTicks = 0;  // script camera behaviour updates since the last tick
        uint32_t cinematicTicks = 0;

        float weight = 0;            // 0 = the game's camera, 1 = the free camera
        bool engaged = false;        // the orbit state is initialized
        float yaw = 0, pitch = RESTING_PITCH, distance = BASE_DISTANCE;
        float yawSign = 1;           // + yaw = towards the camera's right (from the game's matrix)
        Vec3 target;
        bool recentering = false;
        float recenterYaw = 0;
        float lift = 0;              // extra pitch over a wall behind the player (smoothed)
        Vec3 heading;                // last direction the player was sent in (recenter)
        bool hasHeading = false;
        bool mounted = false;        // logged state
        bool r3Down = false;
        int64_t lastTime{};
        const char* reason = "";     // why it is not active (log)
        bool loggedActive = false;
    } g;

    // World ray test from `from` to `to`; the distance to the first hit, or -1.
    float RayTest(PPCContext& ctx, uint8_t* base, const Vec3& from, const Vec3& to)
    {
        uint32_t collision = Manager(COLLISION_MANAGER);
        if (!ValidGuest(collision))
            return -1;
        PPCContext saved = ctx;
        uint32_t sp = (ctx.r1.u32 - 512) & ~15u;
        uint32_t ray = sp + 128, hit = sp + 256;
        StoreV(ray, from);
        StoreV(ray + 12, to);
        StoreBE32(ray + 24, 0xFFFFFFFF);
        StoreBE32(ray + 28, 0xFFFFFFFF);
        StoreBE32(ray + 32, 0);
        for (int i = 0; i < 16; i++)
            StoreBE32(hit + i * 4, 0);
        ctx.r1.u64 = sp;
        ctx.r3.u64 = collision + 4;
        ctx.r4.u64 = ray;
        ctx.r5.u64 = hit;
        ctx.r6.u64 = 1;
        sub_821C5EA0(ctx, base);
        bool result = (ctx.r3.u32 & 0xFF) != 0;
        float distance = LoadF(hit);
        ctx = saved;
        return result && std::isfinite(distance) ? distance : -1.0f;
    }

    // Free distance from `target` towards `direction` (normalized), up to `wanted`: the line of
    // sight and four rays around it (a thin "camera radius").
    float CollideDistance(PPCContext& ctx, uint8_t* base, const Vec3& target, const Vec3& direction, float wanted)
    {
        Vec3 right = Normalize(Cross({ 0, 1, 0 }, direction), { 1, 0, 0 });
        Vec3 up = Cross(direction, right);
        const Vec3 offsets[5] = { {}, right * RADIUS, right * -RADIUS, up * RADIUS, up * -RADIUS };
        float free = wanted;
        for (const Vec3& offset : offsets)
        {
            Vec3 from = target + offset;
            float hit = RayTest(ctx, base, from, from + direction * (wanted + COLLISION_MARGIN));
            if (hit >= 0)
                free = std::min(free, hit - COLLISION_MARGIN);
        }
        return std::max(free, MIN_DISTANCE);
    }

    void HandBack(const char* reason)
    {
        if (g.reason != reason)
        {
            if (g.loggedActive || Probe())
                Log("[camera] free camera: game camera (%s)", reason);
            g.reason = reason;
        }
        g.loggedActive = false;
    }

    bool PlayerValid()
    {
        if (!ValidGuest(g.player) || LoadBE32(g.player) != g.playerVtable)
            return false;
        uint32_t matrix = LoadBE32(g.player + 44);
        return ValidGuest(matrix);
    }

    Vec3 PlayerPosition()
    {
        return LoadV(LoadBE32(g.player + 44) + 48);
    }

    Vec3 PlayerFacing()
    {
        return LoadV(LoadBE32(g.player + 44) + 32);
    }

    // After the game wrote its camera: replace it with the orbit (blended in and out).
    void Update(PPCContext& ctx, uint8_t* base, uint32_t controller, float gameDt)
    {
        int64_t now, frequency;
        now = int64_t(plat::PerfCounter());
        frequency = int64_t(plat::PerfFrequency());
        float dt = g.lastTime ? float(double(now - g.lastTime) / double(frequency)) : 0.0f;
        g.lastTime = now;
        dt = std::clamp(dt, 0.0f, 0.1f);
        bool paused = !(gameDt > 0.0f);

        g.tick++;
        uint32_t scriptCameras = g.scriptCameraTicks, cinematics = g.cinematicTicks;
        g.scriptCameraTicks = g.cinematicTicks = 0;

        if (controller != g.controller)
        {
            // A new level / camera: start over.
            g.controller = controller;
            g.player = 0;
            g.hasHeading = false;
            g.weight = 0;
            g.engaged = false;
        }

        const settings::Values values = settings::Get();
        uint32_t renderManager = Manager(RENDER_MANAGER);
        uint32_t view = ValidGuest(renderManager) ? LoadBE32(renderManager + 356) : 0;

        const char* reason = "";
        if (!values.freeCamera && !ForcedOn())
            reason = "option off";
        else if (!ValidGuest(view))
            reason = "no view camera";
        else if (!PlayerValid())
            reason = "no player";
        else if (g.tick - g.playerSeenTick > 300)
            reason = "player not controlled";
        else if (scriptCameras)
            reason = "script camera";
        else if (cinematics)
            reason = "cinematic";
        bool on = reason[0] == 0;

        float step = dt / BLEND_SECONDS;
        g.weight = std::clamp(g.weight + (on ? step : -step), 0.0f, 1.0f);
        if (!on)
        {
            HandBack(reason);
            if (g.weight <= 0.0f || !ValidGuest(view) || !PlayerValid())
            {
                g.weight = 0;
                g.engaged = false;
                g_active = false;
                TakeMouse(false);
                return;
            }
        }

        Vec3 gamePosition = LoadV(view + 216);
        Vec3 gameDirection = Normalize(LoadV(view + 228));
        // Riding a titan: the player's input drives the titan while the player's own ground
        // behaviour stops; the player's actor rides on its shoulders. Step back. (A long fall
        // also counts after 1.5 s: the camera only backs out a little, smoothly.)
        bool mounted = g.tick - g.groundTick > MOUNTED_TICKS;
        Vec3 targetNow = PlayerPosition() + Vec3{ 0, mounted ? MOUNTED_HEIGHT : TARGET_HEIGHT, 0 };

        if (!g.engaged)
        {
            // Start from where the game's camera is, so switching is seamless.
            Vec3 look = Normalize(targetNow - gamePosition, gameDirection);
            g.yaw = std::atan2(look.x, look.z);
            g.pitch = std::clamp(std::asin(std::clamp(-look.y, -1.0f, 1.0f)), PITCH_MIN, PITCH_MAX);
            g.distance = std::clamp(Length(gamePosition - targetNow), MIN_DISTANCE, BASE_DISTANCE * 2.0f);
            g.target = targetNow;
            // Which way + yaw turns relative to the game's camera right vector (row 0 of the
            // controller's matrix, the camera->world the game itself built).
            uint32_t matrix = LoadBE32(controller + 168);
            Vec3 right = ValidGuest(matrix) ? LoadV(matrix) : Vec3{ 1, 0, 0 };
            Vec3 dYaw{ std::cos(g.yaw), 0, -std::sin(g.yaw) };
            g.yawSign = Dot(right, dYaw) >= 0 ? 1.0f : -1.0f;
            g.recentering = false;
            g.lift = 0;
            g.engaged = true;
        }

        if (on && !g.loggedActive)
        {
            Log("[camera] free camera: active (player actor %08X, yaw %.0f, pitch %.0f, distance %.1f, yaw sign %+.0f)", g.player, g.yaw / DEG,
                g.pitch / DEG, g.distance, g.yawSign);
            g.loggedActive = true;
            g.reason = "";
        }
        g_active = on;
        TakeMouse(on);

        // Input: right stick and mouse, R3 recenters behind the player.
        float sensitivity = std::max(values.cameraSensitivity, 10) / 100.0f;
        float invert = values.cameraInvertY ? -1.0f : 1.0f;
        // Mouse: raw counts, with the [controls] mouse sensitivity too; its invert_y is the
        // mouse's own, on top of the camera's.
        float mouseX = 0, mouseY = 0;
        if (g_mouseTaken)
            input::pc::MouseLookDelta(mouseX, mouseY);
        mouseX *= values.controls.mouseSensitivity / 100.0f;
        mouseY *= values.controls.mouseSensitivity / 100.0f * (values.controls.invertY ? -1.0f : 1.0f);
        float stickX = Channel(g.playerInput, 2), stickY = Channel(g.playerInput, 3);
        float magnitude = std::sqrt(stickX * stickX + stickY * stickY);
        if (magnitude < STICK_DEADZONE)
            stickX = stickY = 0;
        else
        {
            float scaled = std::min((magnitude - STICK_DEADZONE) / (1.0f - STICK_DEADZONE), 1.0f) / magnitude;
            stickX *= scaled;
            stickY *= scaled;
        }
        // R3 from controller 0 itself: the input map's button channels follow the current
        // control scheme (R3 showed up as channel 12 in one run and 15 in another).
        bool r3 = (input::MenuButtons() & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        if (on && !paused)
        {
            float yawInput = stickX * YAW_SPEED * dt + mouseX * MOUSE_DEG_PER_PIXEL;
            float pitchInput = -stickY * PITCH_SPEED * dt + mouseY * MOUSE_DEG_PER_PIXEL;
            if (yawInput != 0 || pitchInput != 0)
                g.recentering = false;
            g.yaw = WrapAngle(g.yaw + yawInput * sensitivity * g.yawSign);
            g.pitch = std::clamp(g.pitch + pitchInput * sensitivity * invert, PITCH_MIN, PITCH_MAX);
            if (r3 && !g.r3Down)
            {
                // Behind where the player last walked (the game's own stick -> world result): the
                // rows of the actor's matrix do not follow its facing reliably.
                Vec3 facing = g.hasHeading ? g.heading : PlayerFacing();
                if (facing.x * facing.x + facing.z * facing.z > 1e-4f)
                {
                    g.recenterYaw = std::atan2(facing.x, facing.z);
                    g.recentering = true;
                    Log("[camera] free camera: recenter behind the player (yaw %.0f -> %.0f)", g.yaw / DEG, g.recenterYaw / DEG);
                }
            }
            if (g.recentering)
            {
                float k = 1.0f - std::exp(-RECENTER_RATE * dt);
                float delta = WrapAngle(g.recenterYaw - g.yaw);
                g.yaw = WrapAngle(g.yaw + delta * k);
                g.pitch += (RESTING_PITCH - g.pitch) * k;
                if (std::fabs(delta) < 0.5f * DEG && std::fabs(RESTING_PITCH - g.pitch) < 0.5f * DEG)
                    g.recentering = false;
            }
        }
        g.r3Down = r3;
        if (Probe())
        {
            static uint32_t previous = 0;
            uint32_t mask = 0;
            for (uint32_t c = 4; c < 32; c++)
                if (Channel(g.playerInput, c) > 0.5f)
                    mask |= 1u << c;
            if (mask != previous)
                Log("[camera] probe: input channels > 0.5: %08X", mask);
            previous = mask;
        }

        // Target point: the player, slightly smoothed (jumps and landings).
        float follow = 1.0f - std::exp(-TARGET_FOLLOW * dt);
        if (Length(targetNow - g.target) > 8.0f)
            g.target = targetNow;  // teleported (checkpoint, respawn)
        else
            g.target = Lerp(g.target, targetNow, follow);

        // Distance with collision: in at once, back out gradually.
        if (mounted != g.mounted && on)
            Log("[camera] free camera: %s", mounted ? "riding (titan): farther away" : "on foot");
        g.mounted = mounted;
        float wantedDistance = BASE_DISTANCE * std::clamp(values.cameraDistance, 25, 300) / 100.0f * (mounted ? MOUNTED_DISTANCE : 1.0f);
        // A wall close behind the player: look from higher up, over it, if that leaves more room
        // (the line of sight from above is usually free; the camera would otherwise sit in the
        // player's head).
        float free = CollideDistance(ctx, base, g.target, DirectionOf(g.yaw, g.pitch) * -1.0f, wantedDistance);
        float wantedLift = 0;
        if (free < wantedDistance * 0.6f)
        {
            float lifted = std::min(g.pitch + LIFT_MAX, PITCH_MAX);
            float freeLifted = CollideDistance(ctx, base, g.target, DirectionOf(g.yaw, lifted) * -1.0f, wantedDistance);
            if (freeLifted > free + 0.5f)
                wantedLift = (lifted - g.pitch) * std::clamp((wantedDistance * 0.6f - free) / (wantedDistance * 0.6f - MIN_DISTANCE), 0.0f, 1.0f);
        }
        g.lift += (wantedLift - g.lift) * (1.0f - std::exp(-LIFT_RATE * dt));
        float pitch = std::min(g.pitch + g.lift, PITCH_MAX);
        Vec3 direction = DirectionOf(g.yaw, pitch);
        if (g.lift > 0.5f * DEG)
            free = CollideDistance(ctx, base, g.target, direction * -1.0f, wantedDistance);
        if (free < g.distance)
            g.distance = free;
        else
            g.distance = std::min(free, g.distance + DISTANCE_RETURN * dt);

        Vec3 position = g.target - direction * g.distance;

        float w = g.weight * g.weight * (3.0f - 2.0f * g.weight);  // smoothstep
        Vec3 finalPosition = Lerp(gamePosition, position, w);
        Vec3 finalDirection = Normalize(Lerp(gameDirection, direction, w), direction);
        StoreV(view + 216, finalPosition);
        StoreV(view + 228, finalDirection);
        StoreV(view + 240, { 0, 1, 0 });
        *g_memory.Translate(view + 208) = 0;  // rebuild the matrices from the vectors

        if (Probe() && g.tick % 30 == 0)
            Log("[camera] probe: %slift %.1f yaw %.1f pitch %.1f dist %.2f (free %.2f, wanted %.2f) w %.2f target %.2f %.2f %.2f cam %.2f %.2f %.2f rs %.2f %.2f%s",
                mounted ? "mounted " : "", g.lift / DEG, g.yaw / DEG, g.pitch / DEG, g.distance, free, wantedDistance, g.weight, g.target.x, g.target.y, g.target.z, finalPosition.x,
                finalPosition.y, finalPosition.z, stickX, stickY, g.recentering ? " recentering" : "");
    }
}

namespace camera
{
    bool FreeCameraActive()
    {
        return g_active;
    }
}

PPC_FUNC(sub_820E0498)
{
    uint32_t self = ctx.r3.u32;
    uint32_t phase = ctx.r6.u32;
    float dt = float(ctx.f1.f64);
    __imp__sub_820E0498(ctx, base);
    if (phase == 3)
        Update(ctx, base, self, dt);
}

PPC_FUNC(sub_820E96F0)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_820E96F0(ctx, base);
    uint32_t inputSlot = LoadBE32(self + 52);
    uint32_t input = ValidGuest(inputSlot) ? LoadBE32(inputSlot) : 0;
    uint32_t actor = LoadBE32(self + 28);
    if (!ValidGuest(input) || !*g_memory.Translate(input + 36) || !ValidGuest(actor))
        return;
    // Keep the actor followed while its ground behaviour runs (co-op: the first one); switch when
    // another controller-driven actor runs and the current one stopped (jacking a titan).
    if (actor != g.player && g.player && PlayerValid() && g.tick - g.playerSeenTick <= 1)
        return;
    if (actor != g.player)
    {
        if (g.player)
            Log("[camera] free camera: following actor %08X (was %08X)", actor, g.player);
        g.player = actor;
        g.playerVtable = LoadBE32(actor);
    }
    g.playerInput = input;
    g.playerSeenTick = g.tick;
    g.groundTick = g.tick;
}


PPC_FUNC(sub_820E63A0)
{
    if (ctx.r6.u32 == 3)
        g.scriptCameraTicks++;
    __imp__sub_820E63A0(ctx, base);
}

PPC_FUNC(sub_821B6EC8)
{
    g.cinematicTicks++;
    __imp__sub_821B6EC8(ctx, base);
}

// While the free camera turns, the camera's forward changes every tick: never hold the old
// walking direction as after a camera cut.
PPC_FUNC(sub_821EBA50)
{
    uint32_t input = ctx.r3.u32;
    __imp__sub_821EBA50(ctx, base);
    if (g_active && ValidGuest(input))
    {
        uint8_t* flags = g_memory.Translate(input + 516);
        *flags &= 0x7F;
    }
}

// The world direction the game made of the left stick: the player's heading for R3, and with
// CRASH_CAMERA_PROBE a log line against the camera.
PPC_FUNC(sub_821EBC70)
{
    uint32_t out = ctx.r4.u32;
    float x = float(ctx.f1.f64), y = float(ctx.f2.f64);
    uint32_t input = ctx.r3.u32;
    __imp__sub_821EBC70(ctx, base);
    if (input == g.playerInput && g.player)
        g.playerSeenTick = g.tick;  // still controlled (walking, airborne, on a titan)
    if (input == g.playerInput && x * x + y * y > 0.09f)
    {
        Vec3 move = LoadV(out);
        if (move.x * move.x + move.z * move.z > 1e-4f)
        {
            g.heading = move;
            g.hasHeading = true;
        }
    }
    if (!Probe() || (x == 0 && y == 0))
        return;
    static uint32_t n = 0;
    if (n++ % 15)
        return;
    Vec3 move = LoadV(out);
    char buttons[160];
    int k = 0;
    for (uint32_t c = 4; c < 24 && k < 150; c++)
        if (Channel(input, c) > 0.5f)
            k += snprintf(buttons + k, sizeof(buttons) - k, " ch%u", c);
    buttons[k] = 0;
    Log("[camera] probe: stick %.2f %.2f -> world %.2f %.2f %.2f (yaw %.1f) camera yaw %.1f%s", x, y, move.x, move.y, move.z,
        std::atan2(move.x, move.z) / DEG, g.yaw / DEG, buttons);
}
