# Mods and assets

Full write-ups: `runtime/NOTES.md` "Mods grandes", "HD AI Remaster"; GUIA "Extraindo e modificando assets",
"Mods de entradas de .rcf", "HD AI Remaster". HD texture packs: [graphics.md](graphics.md).

## `.rcf` overlay and the old 2 GB limit (`fs/rcf_overlay.cpp`, `fs/xenon_drive_hooks.cpp`)

- Mods replace `.rcf` entries; the overlay builds a virtual archive over `default.rcf` (1.2 GB).
- Symptom of the old limit: past ~2 GB the game called `XamShowDirtyDiscErrorUI` at about frame 300 and then
  crashed in sub_823EA7E8 (write to guest 0x50). **No file error in the log**, so it looks like a memory
  problem.
- Cause: the game's own reader `core::XenonDrive::vf14` (sub_82296ED0) seeks with a signed 32-bit
  `SetFilePointer`. Not the `.rcf` format (u32 offsets).
- Fix: `xenon_drive_hooks.cpp` overrides that reader for offsets >= 2 GB; the overlay packs entries compactly
  (own slot if it fits, then freed space, biggest first, then appended). Limit now **4 GB** of virtual archive;
  entries past it are dropped with a WARNING. Check the log line `rcf overlay: ... virtual size`.

## HD AI Remaster (`settings/hd_remaster.cpp`)

- `[graphics] hd_remaster` (off by default) loads the remaster from `<data>\remaster_full\mods\remaster_full`
  via `vfs::AddMod`; it is never copied to `mods\`. Renaming that folder breaks the option
  (`REMASTER_SUBDIRS`). The pack is a release asset, never in git.
- Meshes with **VRTX vertex animation** (water, waterfalls, waves, cloud panel, skybox) must stay original:
  subdividing them crashes `pure3d::VertexAnimController`.
- Level room meshes use textures from shared packages that the level's package list doesn't name: use a
  texture dump (`graphics.texture_dump`) + `tools/hd_textures.py match` to find what is really drawn.
- 256-colour textures needed the package-mod path: HD-pack keys predicted from the exports never matched them
  in the game (cause not investigated).

## Model export (`tools/models.py`)

- The game's triangles are **clockwise-front** for meshes AND skins. The exporter reverses the indices (the
  build side is untouched; round trips are byte-identical).
- Cinematic Crash (`cinematicas/Crash_512`) has the same UV layout as gameplay `crash_normal`, useful for
  texture upscales.

## Audio

- In the `vo_nis` 5.1 files only the centre channel differs between the 6 languages; the minimum-magnitude
  STFT bin across languages gives a clean centre without dialogue.
- Loose-file sound replacement: `audio/audio_override.*` (NOTES "Mods de áudio").
