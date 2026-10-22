#pragma once

#include <cstdint>
#include <vector>

// Sound replacement with loose WAV/Ogg Vorbis files from mods (there is no open XMA encoder).
//
// tools/audio.py build --mod NAME turns mods\NAME\audio\<name>.wav|.ogg into a stand-in .rsd
// (mods\NAME\rcf\<archive>\<entry>, served by fs/rcf_overlay.cpp) whose XMA2 packets hold marker
// frames instead of audio: 15-bit length (128), magic "CRWV", 32-bit sound id, 8-bit stream index,
// 24-bit frame number. The title streams that file to the XMA hardware like any other sound; when
// the decoder (audio/xma.cpp) meets a marker frame, it asks this module for the 512 samples of
// that frame, taken from the loose file listed under that id in mods\NAME\audio\overrides.txt
// (lines "<id hex>\t<file relative to audio\>\t<archive>/<entry>\t<channels>\t<rate>", written by
// the same tool). The file is decoded once, converted to the .rsd's channel count and rate
// (linear resampling) and cached. The first mod in load order that lists an id wins.
namespace audio_override
{
    // If the `bitLength`-bit frame at `frameBits` (MSB first, starting with its length field) is
    // a marker, appends SAMPLES_PER_FRAME (512) samples x `channels` of 16-bit PCM to `pcm` and
    // returns true. Unknown ids and files that fail to load give silence (logged once).
    bool DecodeMarkerFrame(const uint8_t* frameBits, uint32_t bitLength, uint32_t channels, std::vector<int16_t>& pcm);
}
