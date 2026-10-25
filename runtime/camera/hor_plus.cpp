// Hor+ for wide screens (gpu/widescreen.h; runtime/NOTES.md "Ultrawide").
//
// pure3d::Camera::vf05 (sub_823176C8) is SetFOV(fov, aspect). The game calls it every frame for
// its cameras (and pure3d::Camera::vf22, which hands the camera to pddi, calls it again) with
// fov = the horizontal FOV in radians (80 degrees in gameplay) and aspect = 1; it stores those
// at +20 / +24 (GetFOV, vf06, returns them) and then derives, with the display's aspect
// (pure3d::xnDisplay::vf20 sub_8238F438: +1688 / +1692 = 16:9):
//
//   +16 effective horizontal FOV, +28 effective aspect (+212 * aspect * display aspect),
//   +40 1 / tan(fov / 2), +44 aspect / tan(fov / 2) (the projection's x and y scales),
//   +48..+76 the side planes of the frustum culling (vf16): left (-cos h, -sin h), right
//   (cos h, -sin h) in x/z, bottom / top (cos v, -sin v) / (-cos v, -sin v) in y/z, with h and v
//   the horizontal and vertical half angles.
//
// vf22 then gives pddi near / far / +16 / +28: pure3d::pddiBaseContext::vf29 (sub_82393980) is
// SetProjection(near, far, fov, aspect), which builds the projection matrix. The HUD's 3D icons
// set their own projections there too (fov 1, aspect 16:9).
//
// Hor+ is done in two places:
// - SetProjection: a 16:9 projection is widened to the picture's aspect with the same vertical
//   FOV (on a narrower screen, 16:10, the same horizontal FOV: more above and below).
// - vf16 (sub_823170F8, the culling's sphere test): tested against side planes widened the same
//   way, so the sides are not culled. The camera itself stays 16:9: other code reads its derived
//   fields (writing the wider FOV, scales or planes into it made the ice shards of the village
//   blink once the camera moved), and +20 / +24 keep what the game asked for.

#include "../gpu/widescreen.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_823170F8);  // pure3d::Camera::vf16 (sphere in the frustum)
extern "C" PPC_FUNC(__imp__sub_82393980);  // pure3d::pddiBaseContext::vf29 (SetProjection(near, far, fov, aspect))
extern "C" PPC_FUNC(__imp__sub_823939A8);  // pure3d::pddiBaseContext::vf30 (GetProjection(&near, &far, &fov, &aspect))
extern "C" PPC_FUNC(__imp__sub_8238D248);  // pure3d::xnContext::vf119 (builds the projection matrix)

namespace
{
    constexpr uint32_t CAMERA_FOV = 16, CAMERA_ASPECT = 28, CAMERA_NEAR = 32, CAMERA_FAR = 36;

    // CRASH_NO_HOR_PLUS=1 (debug): the 16:9 view, stretched to the picture.
    const bool g_disabled = getenv("CRASH_NO_HOR_PLUS") != nullptr;

    float LoadF(uint32_t guest)
    {
        uint32_t v = LoadBE32(guest);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }

    void StoreF(uint32_t guest, float value)
    {
        uint32_t v;
        memcpy(&v, &value, 4);
        StoreBE32(guest, v);
    }

    // The tangents of the half angles of a 16:9 view (horizontal FOV `fov`) at the picture's aspect.
    // False when there is nothing to change.
    bool Widen(float fov, float gameAspect, float& tanH, float& tanV)
    {
        float aspect = gpu::widescreen::Aspect();
        if (g_disabled || fabsf(gameAspect - gpu::widescreen::BASE_ASPECT) > 0.01f || fabsf(aspect - gameAspect) < 0.001f ||
            !(fov > 0.01f && fov < 3.1f))
            return false;
        tanH = tanf(fov * 0.5f);
        tanV = tanH / gameAspect;
        if (aspect > gameAspect)
            tanH = tanV * aspect;  // Hor+: same vertical FOV
        else
            tanV = tanH / aspect;  // narrower: same horizontal FOV
        return true;
    }

    int g_logged = 0;

    // The last projection widened (SetProjection): what the game asked for and what pddi got. The
    // state (context +136) keeps near +36, far +40, fov +44, aspect +48 and the projection mode
    // +16 (0 perspective, 1 orthographic, 2 pixels); vf119 builds the matrix of the mode. Only the
    // perspective is widened: the orthographic one (the HUD's 2D) is made from the aspect too, and
    // with the widened one the HUD slid down, off the screen at 32:9. GetProjection gives the game
    // its own values back.
    struct Projection
    {
        uint32_t context = 0;
        float askedFov = 0.0f, askedAspect = 0.0f, givenFov = 0.0f, givenAspect = 0.0f;
    };
    thread_local Projection g_projection;
}

// vf16: is the sphere (r4: camera space x, y, z; f1: radius) in the frustum? The original's test
// with the widened side planes (the camera's own planes stay 16:9).
PPC_FUNC(sub_823170F8)
{
    uint32_t self = ctx.r3.u32;
    thread_local float cachedFov = 0.0f, cachedAspect = 0.0f, cachedPicture = 0.0f;
    thread_local bool cachedWide = false;
    thread_local float planes[8];
    float fov = LoadF(self + CAMERA_FOV), gameAspect = LoadF(self + CAMERA_ASPECT), picture = gpu::widescreen::Aspect();
    if (fov != cachedFov || gameAspect != cachedAspect || picture != cachedPicture)
    {
        cachedFov = fov, cachedAspect = gameAspect, cachedPicture = picture;
        float tanH, tanV;
        cachedWide = Widen(fov, gameAspect, tanH, tanV);
        float h = atanf(tanH), v = atanf(tanV);
        float values[8] = { -cosf(h), -sinf(h), cosf(h), -sinf(h), cosf(v), -sinf(v), -cosf(v), -sinf(v) };
        memcpy(planes, values, sizeof(planes));
    }
    if (!cachedWide)
    {
        __imp__sub_823170F8(ctx, base);
        return;
    }
    uint32_t point = ctx.r4.u32;
    float x = LoadF(point), y = LoadF(point + 4), z = LoadF(point + 8), r = float(ctx.f1.f64);
    bool inside = z + r >= LoadF(self + CAMERA_NEAR) && !(z - r > LoadF(self + CAMERA_FAR)) &&
                  !(planes[0] * x + planes[1] * z > r) && !(planes[2] * x + planes[3] * z > r) &&
                  !(planes[4] * y + planes[5] * z > r) && !(planes[6] * y + planes[7] * z > r);
    ctx.r3.u64 = inside ? 1 : 0;
}

PPC_FUNC(sub_82393980)
{
    float fov = float(ctx.f3.f64), gameAspect = float(ctx.f4.f64);
    float tanH, tanV;
    if (Widen(fov, gameAspect, tanH, tanV))
    {
        float aspect = gpu::widescreen::Aspect();
        ctx.f3.f64 = 2.0 * atan(double(tanH));
        ctx.f4.f64 = aspect;
        g_projection = { ctx.r3.u32, fov, gameAspect, float(ctx.f3.f64), aspect };
        if (g_logged < 2)
        {
            g_logged++;
            Log("[aspect] projection: FOV %.1f x %.1f degrees at 16:9 -> %.1f x %.1f at %.3f", fov * 57.29578f,
                2.0f * atanf(tanf(fov * 0.5f) / gameAspect) * 57.29578f, 2.0f * atanf(tanH) * 57.29578f, 2.0f * atanf(tanV) * 57.29578f, aspect);
        }
    }
    __imp__sub_82393980(ctx, base);
}

// GetProjection: the values the game set, not the widened ones.
PPC_FUNC(sub_823939A8)
{
    uint32_t self = ctx.r3.u32, fovOut = ctx.r6.u32, aspectOut = ctx.r7.u32;
    __imp__sub_823939A8(ctx, base);
    const Projection& p = g_projection;
    if (p.context == self && fovOut && aspectOut && LoadF(fovOut) == p.givenFov && LoadF(aspectOut) == p.givenAspect)
    {
        StoreF(fovOut, p.askedFov);
        StoreF(aspectOut, p.askedAspect);
    }
}

// vf119: the orthographic and pixel matrices with the values the game set.
PPC_FUNC(sub_8238D248)
{
    uint32_t self = ctx.r3.u32;
    uint32_t state = LoadBE32(self + 136);
    const Projection& p = g_projection;
    bool restore = p.context == self && state && LoadBE32(state + 16) != 0 && LoadF(state + 44) == p.givenFov &&
                   LoadF(state + 48) == p.givenAspect;
    if (restore)
    {
        StoreF(state + 44, p.askedFov);
        StoreF(state + 48, p.askedAspect);
    }
    __imp__sub_8238D248(ctx, base);
    if (restore)
    {
        StoreF(state + 44, p.givenFov);
        StoreF(state + 48, p.givenAspect);
    }
}
