#include "widescreen.h"
#include "../settings/settings.h"

#include <atomic>
#include <cmath>

namespace gpu::widescreen
{
    namespace
    {
        std::atomic<float> g_aspect{ BASE_ASPECT };
    }

    float Aspect()
    {
        return g_aspect.load(std::memory_order_relaxed);
    }

    void SetAspect(float aspect)
    {
        g_aspect = aspect > 0.5f && aspect < 8.0f ? aspect : BASE_ASPECT;
    }

    float GameAspect()
    {
        return fabsf(Aspect() - ASPECT_4_3) < 0.01f ? ASPECT_4_3 : BASE_ASPECT;
    }

    void HudArea(uint32_t width, uint32_t height, float& x, float& y, float& areaWidth, float& areaHeight)
    {
        float w = float(width), h = float(height);
        areaWidth = w;
        areaHeight = h;
        if (w > h * BASE_ASPECT * 1.001f)
            areaWidth = h * BASE_ASPECT;
        else if (w < h * BASE_ASPECT * 0.999f)
            areaHeight = w / BASE_ASPECT;
        x = (w - areaWidth) * 0.5f;
        y = (h - areaHeight) * 0.5f;
    }

    void OverlayArea(uint32_t width, uint32_t height, float& x, float& y, float& areaWidth, float& areaHeight)
    {
        float w = float(width), h = float(height), aspect = Aspect();
        x = y = 0.0f;
        areaWidth = w;
        areaHeight = h;
        if (GameAspect() == BASE_ASPECT)
            return;
        if (w > h * aspect + 0.5f)
            areaWidth = std::round(h * aspect);
        else if (h > w / aspect + 0.5f)
            areaHeight = std::round(w / aspect);
        x = std::floor((w - areaWidth) * 0.5f);
        y = std::floor((h - areaHeight) * 0.5f);
    }
}
