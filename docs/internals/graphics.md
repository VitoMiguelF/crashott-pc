# Graphics

Full write-ups: `runtime/NOTES.md` steps 10 and 16, "Passo 20" (PC graphics options), "Mods grandes" (HD
textures), "Iluminação (remaster)", "Janela". Test profiles: `iluminacao`, `iluminacao_tela`,
`configuracoes_resolucao`, plus `CRASH_SETTINGS` overrides on `primeiro_slot_load` / `fase_direta`.

## Renderer facts (`runtime/gpu/`)

- Shaders: Xenos microcode -> HLSL -> FXC SM5.1 (`shader_translator`). `--test-shaders <dir>` translates
  every shader. Debug: `CRASH_GPU_TRACE_SWAP`, `CRASH_GPU_SKIP_PS=<hashes>`, `CRASH_GPU_SHOW_RESOLVE`,
  `userdata\shaderoverride\`.
- VS epilogue: reciprocate W only when `vtx_w0_fmt = 0`. MULSC/ADDSC/SUBSC's constant operand is always
  `c[src3_reg]` (`src3_sel` is part of the temp index).
- Xenos `face = 0` means **CW is front** (inverting it darkened the screen through the stencil-shadow pass).
- Present must apply the front-buffer fetch constant swizzle (0xA0A = ZYX1): D3D resolves the front buffer
  with R/B swapped.
- Render targets are keyed by row width in samples: a 4x MSAA view of a 1x RT scales rows by 2 (D3D
  fast-clears the 1280x720 1x depth through a 640-pitch 4x surface). Depth resolves also write packed RGBA8
  (R = stencil) for k_8_8_8_8 fetches (stencil shadows).
- Motion blur: D3D clears the velocity RT (EDRAM 1080) per tile with a depth-only ZFUNC ALWAYS rect on a 4x
  surface (stencil ref 0x80 -> EDRAM word 0x00808080 = neutral velocity). The renderer also draws such
  depth-only clears into the k_8_8_8_8 colour RT at that base (`DEPTH_ALIAS_PS`; `CRASH_GPU_NO_DEPTH_ALIAS=1`
  = old behaviour). General EDRAM transfers are still missing.
- **Tiling**: tiles are 448 px wide overlapping 32 (scissor 0-448 / 416-864 / 832-1280). Each tile resolves
  the scene into 0x0A642000 without a clear before refraction, and its final with a clear into the same
  texture, so tile 3 read tile 2's final (ice band at x=832). `Resolve()` keeps per-surface `pre`/`final`
  stash copies (`TileStashBeforeResolve`; `CRASH_GPU_NO_TILE_STASH=1` disables).
- The orange translucent shapes in the top corners are the P1/P2 HUD, not artifacts.

## PC graphics options (`[graphics]`, `gpu/graphics_options.cpp`)

- DLSS 3.x (static `nvsdk_ngx_s.lib`) and FSR 3.1 through the FidelityFX API, upscaling only; SMAA; mipmaps +
  SampleGrad (derivatives of the pristine PS input registers taken in `main()`); motion blur / DOF / shadow
  softness through HLSL patches fed by root constants b3.
- **Scene vs output**: scene = render targets first used with 2x/4x MSAA (`g_sceneScale`); 1x targets are
  post/HUD (output scale). Resolve targets keep one surface per scale.
- The upscaler runs before the first 1x draw that samples this frame's scene colour resolve (0x0A642000 in a
  level).
- Scene VS `aecab5202246c470` / `429975a046a1a236`: c4-c7 MVP, c12-c15 model, c16-c19 previous MVP,
  c255.x = velocity K = 5. Velocity RT alpha = 1 where objects wrote; x is in blue after the resolve swap.
- Depth is inverted (z = -ndc + 1).
- Resolution / upscaler / quality apply live when the settings screen closes
  (`RequestReconfigure` -> `Reconfigure()` at the end of Swap). The first time DLSS is shown, its probe stalls
  the render thread ~1.2 s (known).

## HD texture packs (`gpu/hd_textures.*`, `tools/hd_textures.py`)

- Packs: `mods\<mod>\hd_textures\<16-hex key>.dds` (BC1/2/3/7 or RGBA8, any mips). Settings
  `[graphics] hd_textures`, `texture_dump`.
- Key = stable hash of the decoded base level at upload (never RTs/resolves). Guest memory untouched; guest
  dims kept for addressing, sampler LOD shifted.
- Game textures are k_8_8_8_8 with swizzle 0x60A (BGRA) and stored **bottom-up**, so the tool predicts keys
  straight from exported PNGs without a dump.
- A texture also rewritten by a package (`.rcf`) mod gets a different key: compute it from the rewritten
  texture (texture_dump + `hd_textures.py match`).
- Palettized textures (k_8 + 256x1 palette at sampler s8, e.g. button icons) are invisible to the RGBA dump;
  their key is the hash of the R8 indices.
- Pitfall: reading back from the D3D12 upload heap (write-combined) cost 1.3 s per level load; decode the base
  level to host memory first.

## Lighting remaster (`gpu/lighting.*`)

- Runs in the renderer's scene pass (`RunTemporal`, also without upscaler via `ScenePassActive`), before
  DLSS/FSR and the HUD: visibility-bitmask SSAO + one-bounce indirect light, contact shadows (rays straight
  up), temporal + bilateral, composite floor 50%. Soft shadows are an alternative path in the shadow shader
  patch (`xe_jitter.z`).
- `[lighting] ssao off|low|high, gi, gi_intensity, contact_shadows, soft_shadows`, all off by default, live.
  **F8** toggles everything (`lighting::ToggleBypass`). Debug: `CRASH_LIGHTING_SHOW=ao|gi|shadow|split`,
  `CRASH_LIGHTING_TUNE`, `CRASH_LIGHTING_NO_TEMPORAL`.
- Facts: the lit shaders' directional lights (c9-c16) are mostly disabled (PS bools dword 4 bits 0-3 = 0) and
  the stencil shadow volumes are CPU-extruded (no light constant), so a key-light contact shadow contradicts
  the game's shadows. Terrain seams are 1-px cracks that need depth healing before AO.
- Real shadow maps were judged not worth it (no reliable light; doubles command-processor work).
- Extend `lighting.cpp` rather than patching game shaders.
- Cost (RTX 5080): 1080p HIGH 0.75 ms GPU, LOW 0.17 ms; 4K HIGH 1.09 ms (AO capped to half res above 1440
  lines); zero CPU.

## Window

The present keeps the aspect ratio with black bars and the process is per-monitor DPI aware (on a high-DPI
screen the window used to be half resolution and stretched).
