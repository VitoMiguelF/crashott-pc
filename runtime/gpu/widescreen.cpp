#include "widescreen.h"
#include "../settings/settings.h"

#include <atomic>

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
}
