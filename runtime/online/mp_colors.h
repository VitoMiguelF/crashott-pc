#pragma once

// Each player's Crash colour in a session (docs/internals/multiplayer.md, "Colours"). The player picks
// one in the MULTIPLAYER screens ([multiplayer] color in settings.toml); a client sends it to the host
// in HELLO / PING, the host gives every slot a colour nobody else has (the one asked for, else the next
// free one) and sends them in the ROSTER, and every machine paints each slot's Crash with it.
//
// The colours are the normal palette (orange), the jacked one (P2's pale fur) and the normal one's fur
// recoloured after the game's palette blend of every frame (hue, saturation, a value curve; the skin,
// the jeans and the shoes stay as they are).
//
//   CRASH_MP_COLOR=<id>        tests: this machine's colour (instead of settings.toml)
//   CRASH_MP_COLORS=<id>,...   tests without a lobby (CRASH_MP_PLAYERS): the colour of slot 1, 2, ...
//   CRASH_MP_COLORS_EVERY=<ms> with CRASH_MP_COLORS: every <ms> the slots take the next ones of the list
//                              (a list of 12 on 4 players shows them all in three steps)

#include <cstdint>
#include <string>

namespace mp::colors
{
    constexpr uint32_t MAX_SLOTS = 8;  // the session's slots this module keeps a colour for
    constexpr uint8_t NONE = 0xFF;

    struct Colour
    {
        const char* id;  // in settings.toml
        const char* en;
        const char* pt;
    };

    uint32_t Count();
    const Colour& Get(uint32_t index);  // index < Count()
    int Find(const std::string& id);     // -1 if unknown
    void Swatch(uint32_t index, uint8_t rgb[3]);  // the fur's mid tone (the menus' sample)

    // This machine's player's colour: settings.toml [multiplayer] color (or CRASH_MP_COLOR).
    uint32_t Mine();
    // The next colour from Mine() in `direction` (+1 / -1), skipping the ones the other players of the
    // session have; saved in settings.toml.
    void Step(int direction);
    // The first colour from `from` (going `direction`) not in `taken` (a bit per colour).
    uint32_t NextFree(uint32_t from, int direction, uint32_t taken);

    // The colours the lobby gave the slots (host: resolved; client: from the roster). Without them
    // (no session) a slot's is CRASH_MP_COLORS's or the default (orange, white, blue, green, red,
    // purple, yellow, pink).
    void SetSlotColours(const uint8_t* colours, uint32_t count);
    void ClearSlotColours();
    uint32_t ForSlot(uint32_t slot);

    // A player's drawable (mp_net.cpp FixLooks): whether its base is the jacked palette, and its fur
    // recoloured from now on (every frame, after the game's blend). Forget when the drawable goes.
    bool UsesJacked(uint32_t colour);
    void Paint(uint32_t drawable, uint32_t colour);
    int PaintedWith(uint32_t drawable);  // -1 if not painted
    void Forget(uint32_t drawable);
}
