#pragma once

// The settings' music / effects / voice volumes, applied live through the game's own volume
// categories (the ones its pause menu's DIALOGUE / MUSIC / SFX stars set, a screen the settings
// now cover): the value stored is the setting's factor, whatever the game sets, and the game still
// reads back its own value. See game_volumes.cpp.
namespace audio::volumes
{
    enum class Category : int { Music = 0, Effects = 1, Voice = 2 };

    // 0..100 each; applied right away if the game's volume object exists, else when it appears.
    void SetPercent(int music, int effects, int voice);
    // Game thread, every controller read: picks up the game's volume object when it is created.
    void Tick();
}
