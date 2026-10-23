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

## Vulkan renderer (`rhi.h`, `rhi_vulkan.cpp`)

- `xenos_renderer.cpp` is API-agnostic: PM4 state, EDRAM render targets, resolves, tile stash, texture
  decode and cache, temporal passes and present. It talks to an `rhi::Device` (`rhi.h`) with two binding
  models: **draws** (Xenos shaders: b0-b3, t0-t31, s0-s31, the upload ring at t0 space1) and **passes**
  (fullscreen helpers: b0, t0-t7 or a structured buffer, static samplers s0 point / s1 linear).
  Formats are DXGI enums (only `<dxgiformat.h>`), mapped by the backend.
- The only backend is `rhi_vulkan.cpp` (Vulkan 1.3: dynamic rendering, synchronization2, a timeline
  semaphore per submission; DXC -> SPIR-V, `<hash>.spv`, `userdata\shadercache\vulkan_pipelines.bin`).
  Direct3D 12 (`rhi_d3d12.cpp`, FXC) was dropped on 2026-10-23 after reaching parity; `rhi.h` keeps the
  shape so a native Linux WSI or Metal only needs a new Device. Without a Vulkan 1.3 device or a
  `dxcompiler.dll` that emits SPIR-V, `renderer::Init` logs why, shows a message box and exits.
- Vulkan facts: viewport flipped (negative height), front faces stay clockwise; draws bind textures as push
  descriptors (set 0), b0-b3 as dynamic UBOs in the ring + the ring as a storage buffer (set 1), sampler
  sets cached by description (set 2); passes use a regular set per pass from the command buffer's pool.
  D24S8 when sampleable (NVIDIA), else D32S8 + `VK_EXT_depth_bias_control` (AMD/RADV, Steam Deck;
  `CRASH_VULKAN_D32=1` forces it). `XE_STENCIL` reads `.x` of a stencil view.
- Pitfalls found: render targets/depth packed at the driver's 1 KB alignment in shared 64 KB pages
  corrupted each other on NVIDIA (attachments get 64 KB alignment); push descriptors with immutable
  samplers sampled zeros from image views created after the first frames (the menus' second front buffer
  presented black every other frame), hence regular sets for passes; the SDK 1.4.363 validation layer
  crashes in `vkCmdPushDescriptorSetKHR` when the push set is not set 0; llvm-mingw needs the DXC
  interface GUIDs declared (`__CRT_UUID_DECL`).
- Upscalers (`upscaler.cpp`) run on the renderer's device: DLSS through NGX's Vulkan API (same static
  `nvsdk_ngx_s.lib`), FSR 3.1.4 through `amd_fidelityfx_vk.dll` (FidelityFX SDK 1.1.4; SDK 2.x is DX12
  only). The device enables the extensions they ask for (`rhi::DeviceInfo`): NGX's list, and the ones
  the FidelityFX DLL looks up by extension name (`vkGetBufferMemoryRequirements2KHR`,
  `vkSetDebugUtilsObjectNameEXT`: a null call otherwise).
- Debug: `CRASH_GPU_DEBUG=1` (validation + sync validation; `run_game.py --set gpu_debug=true`),
  `CRASH_VULKAN_DEVICE=<name part>`, `CRASH_VULKAN_TRACE=N`, `CRASH_VULKAN_TRACE_PASSES=<submit>`,
  `CRASH_VULKAN_DEDICATED=1`, `CRASH_GPU_TRACE_SWAPS=N`, `CRASH_GPU_TRACE_RESOLVE=<hex>`,
  `CRASH_GPU_DUMP_RESOLVE=<hex>`, `CRASH_GPU_DUMP_PRESENT=1`. Status, parity table and the native-Linux
  plan: `runtime/NOTES.md`, "Vulkan".

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
- Pitfall: reading back from the upload heap (write-combined) cost 1.3 s per level load; decode the base
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
