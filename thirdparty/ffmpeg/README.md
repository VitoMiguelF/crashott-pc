# FFmpeg (LGPL) for the XMA decoder

Subset of the BtbN FFmpeg build `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`
(https://github.com/BtbN/FFmpeg-Builds/releases, linked from ffmpeg.org's download page),
downloaded 2026-10-19:

- `include/libavcodec`, `include/libavutil`: headers;
- `bin/avcodec-62.dll`, `bin/avutil-60.dll`, `bin/swresample-6.dll`: shared libraries;
- `LICENSE.txt`: the license of this build (LGPL v3).

`runtime/audio/ffmpeg.cpp` loads the DLLs at run time (`LoadLibrary`); nothing links statically
against FFmpeg, and the DLLs can be swapped for another build of the same major versions
(avcodec 62 / avutil 60) that has the WMA Pro decoder. The CMake build copies `bin/*.dll` next to
`CrashRecomp.exe`. If this folder is missing, the runtime still builds and XMA decodes to silence.

Source code for this FFmpeg version: https://github.com/FFmpeg/FFmpeg/tree/n8.1 (build scripts:
https://github.com/BtbN/FFmpeg-Builds). Anyone redistributing these DLLs must follow the LGPL
(ship the license and offer the corresponding source).
