// The parts of platform.h that need a window system outside Windows, through SDL3.
#include "platform.h"

#include <SDL3/SDL.h>

#include <cmath>

namespace plat
{
    bool DesktopDisplayMode(int& width, int& height, int& refreshHz)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
            return false;
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        bool ok = mode && mode->w > 0 && mode->h > 0;
        if (ok)
        {
            // Physical pixels, like EnumDisplaySettings (Wayland reports points and a density).
            float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
            width = int(std::lround(float(mode->w) * density));
            height = int(std::lround(float(mode->h) * density));
            refreshHz = int(std::lround(mode->refresh_rate));
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);  // reference counted: the window's own init stays
        return ok;
    }
}
