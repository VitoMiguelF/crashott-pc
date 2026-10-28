#pragma once

// The skin challenges' progress (challenge_counters.cpp): in the HUB's locked dolls
// ("DEFEAT 87/200 RATNICIANS") and, in a level, a counter under the game's own minion counter each
// time one of those enemies is defeated / jacked.

#include <cstdint>

namespace settings::challenges
{
    // gpu/xenos_renderer.cpp, at present time: the in-level counters.
    void Draw(uint32_t width, uint32_t height);
}
