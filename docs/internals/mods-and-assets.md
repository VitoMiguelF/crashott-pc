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

## Blue Snipe shots (`settings/snipe_colors.cpp`)

Full write-up: `runtime/NOTES.md` "Snipe azul atirando rosa"; GUIA "Tiro do Snipe azul". Profile `snipe_azul`.

- A game bug, not a port bug: the Snipe (Parafox) picks a palette variant at random when its drawable is set
  up (`sub_8221F4A0`, called from `CCharacterRenderBehaviour::vf12` sub_82149FF8): pink (`Parafox_normal` /
  `_jacked`) or blue (`Parafox_normal_A` / `_jacked_A`). Variant index at character drawable `+4548`;
  normal palettes `+4516`, jacked `+4532` (4 each). The drawable is the `CCharacterRenderBehaviour`'s `+92`;
  that behaviour is at actor `+340` (vtable `0x82015BC4`). Lua `SetCharacterSkin` (sub_82149B08) writes
  `+4500`, something else.
- The shot (`Weapons:Projectile:ParafoxProjectile`, `package\c4ec64ea.p3d`) is pink for everyone: an RGB copy
  of the texture pre-painted with the pink palette, pink particle keys, pink billboards, light (255, 47, 151).
- Projectiles come from a per-launcher quiver: `LoadProjectile` sub_8212F238 (launcher +288 quiver, +304
  owner actor) -> next projectile sub_8212EE60 (r4 = owner) -> while not full, sub_821363E8(genName,
  template, owner, ...) -> Lua `GenerateObject` -> `TemplateActorFunctions[genName]`. Then reused round-robin.
  The template hash (r4) is what the stream manager uses; genName (r3) is only the Lua key and the spawn name.
- Fix: a package rewrite appends blue copies of the feather, trail and impact chunks (same-length renames
  `parafox` -> `parablu`, `Parafox` -> `Parablu`; prop sound callbacks 0x8020030 keep the original name;
  the prop's collision 0x8020020 and physics object 0x5000000 are copied too).
  Hooks on sub_8212EE60 + sub_821363E8 swap genName to `...ParafoxProjectileBlue` for a variant-1 owner; that
  Lua template (defined at first use through `dev::RunLua`) runs the original with the global `newActor`
  wrapped once, then sets `drawable_entity`, `physical_ellipsoid_text`, `intersection_volume_text`,
  `default_hit_effect_entity` and `colour_colour`.
- Trap: a renamed state prop must keep its own collision. With the drawable renamed but the collision /
  physics names left pink (and 0x8020020 / 0x5000000 not copied) the shot hit only the ground and went
  through characters and targets. All three names must point at a full copy. Check: `[dev] invincible:
  blocked` lines (each a shot that hit an invincible Crash).
- Hooks owned by this file: sub_8212EE60, sub_821363E8, sub_8221F4A0 (`CRASH_FORCE_SKIN_VARIANT`, tests).
- Not covered: the Snipe's own hand effects (heavy / special attack), in the character package `e387b81a`.
- Recolouring a truecolour PNG inside a package changes its size: rebuild the chunk tree (sizes of every
  ancestor), not an in-place patch. Stored-deflate PNGs (type 2/6) load fine in the game.
