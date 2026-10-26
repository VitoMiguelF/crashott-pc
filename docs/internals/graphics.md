# Graphics

Full write-ups: `runtime/NOTES.md` steps 10 and 16, "Passo 20" (PC graphics options), "Mods grandes" (HD
textures), "Iluminação (remaster)", "Janela". Test profiles: `iluminacao`, `iluminacao_tela`,
`configuracoes_resolucao`, plus `CRASH_SETTINGS` overrides on `primeiro_slot_load` / `fase_direta`.

## Renderer facts (`runtime/gpu/`)

- Shader warm-up (`shader_warmup.*`): the runtime records each translated microcode in `shadercache\ucode\` and
  compiles both variants (720p / scaled) of every known shader at start on a low-priority thread;
  `--precompile-shaders` (run by `tools/setup.py`) does it on every core. Most vertex shaders are made at run
  time by D3D's vertex-declaration binding: they are not in the game files.
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
- **Single-pass scene** (`gpu/tiling_hooks.cpp`, the only path): the console renders the 2x MSAA scene in 3
  predicated tiles (448 px, overlapping 32) and D3D queues the scene until EndTiling. Here the scene targets
  are created at 1280x720 with explicit EDRAM bases 1440/2048/2880, BeginTiling is skipped, EndTiling becomes
  one Resolve and the xn per-tile resolves collapse into one (2.4x fps in the village). The tile stash that
  fixed the ice band at the x = 832 seam went with the tiling path (no seams). Details:
  [native-gpu-plan.md](native-gpu-plan.md) 3.3.
- The orange translucent shapes in the top corners are the P1/P2 HUD, not artifacts.

## Vulkan renderer (`rhi.h`, `rhi_vulkan.cpp`)

- `xenos_renderer.cpp` is API-agnostic: PM4 state, EDRAM render targets, resolves, texture
  decode and cache, temporal passes and present. It talks to an `rhi::Device` (`rhi.h`) with two binding
  models: **draws** (Xenos shaders: b0-b3, t0-t31, s0-s31, the upload ring at t0 space1) and **passes**
  (fullscreen helpers: b0, t0-t7 or a structured buffer, static samplers s0 point / s1 linear).
  Formats are DXGI enums (only `<dxgiformat.h>`), mapped by the backend.
- The only backend is `rhi_vulkan.cpp` (Vulkan 1.3: dynamic rendering, synchronization2, a timeline
  semaphore per submission; DXC -> SPIR-V, `<hash>.spv`, `userdata\shadercache\vulkan_pipelines.bin`,
  `pipeline_keys.bin`).
  Direct3D 12 (`rhi_d3d12.cpp`, FXC; in git history) was removed on 2026-10-23 after reaching parity; `rhi.h` keeps the
  shape so a native Linux WSI or Metal only needs a new Device. Without a Vulkan 1.3 device or a
  `dxcompiler.dll` that emits SPIR-V, `renderer::Init` logs why, shows a message box and exits.
- Vulkan facts: viewport flipped (negative height), front faces stay clockwise; draws bind textures as push
  descriptors (set 0), b0-b3 as dynamic UBOs in the ring + the ring as a storage buffer (set 1), sampler
  sets cached by description (set 2); passes use a regular set per pass from the command buffer's pool.
  D24S8 when sampleable (NVIDIA), else D32S8 + `VK_EXT_depth_bias_control` (AMD/RADV, Steam Deck;
  `CRASH_VULKAN_D32=1` forces it). `XE_STENCIL` reads `.x` of a stencil view.
- A draw whose attachments are a subset of the open rendering instance's is recorded inside it with a pipeline
  variant (`keepFormats`: the instance's formats, extra slots write-masked, depth/stencil off without depth):
  71 -> 54 instances per village frame (the stats line gives why the remaining ones were begun).
- Pitfalls found: render targets/depth packed at the driver's 1 KB alignment in shared 64 KB pages
  corrupted each other on NVIDIA (attachments get 64 KB alignment); push descriptors with immutable
  samplers sampled zeros from image views created after the first frames (the menus' second front buffer
  presented black every other frame), hence regular sets for passes; the SDK 1.4.363 validation layer
  crashes in `vkCmdPushDescriptorSetKHR` when the push set is not set 0; llvm-mingw needs the DXC
  interface GUIDs declared (`__CRT_UUID_DECL`).
- **Pipeline compilation** (`[graphics] shader_compilation`, `rhi::ShaderCompilation`; `runtime/NOTES.md`
  "Compilação de shaders assíncrona"). A draw's `DrawPipelineKey` maps to a `Program` (`ProgramKey`: the
  shaders by `vulkan::SpirvHash` + the state that isn't dynamic: formats, blend, write mask, topology class,
  depth clip). Cull mode, depth/stencil tests and ops, stencil masks, depth bias (D24-on-D32 through
  `vkCmdSetDepthBias2EXT`), topology and primitive restart are Vulkan 1.3 dynamic state (`DrawDynamic`,
  `SetDrawDynamic`): the village needs 50 programs instead of 86 pipelines.
  - `async` (default): with `VK_EXT_graphics_pipeline_library` + fast linking (enabled whenever present),
    a program is fast-linked from four parts (vertex input by topology class, pre-rasterization by VS/GS/
    depth clip, fragment shader by PS, fragment output by formats/blend/mask; `LibraryKey`), each compiled
    once; a worker then links it with link-time optimization and `Program::Current()` switches to that.
    Workers (`hardware_concurrency / 4`, 1-4, below normal) also compile at start every pipeline in
    `shadercache\pipeline_keys.bin` (80-byte `ProgramKey` records appended by `SavePipelineCache`; the last
    8192) and, from the shader warm-up, the VS/PS parts of every known shader (`PrewarmShader`). A notice
    "PREPARANDO SHADERS... n%" shows when that takes over 3 s (a new driver).
  - `sync`: whole pipelines on the recording thread when first drawn (the old behaviour, with dynamic state).
  - `aggressive` (settings.toml only): async, and a draw that can wait (scene MSAA target, colour only, no
    depth/stencil write, not a rectangle-list clear nor a depth-alias clear) is skipped while its parts or
    pipeline compile (`SKIP_COMPILING`).
  - `Lazy` (idle / queued / building / built): whoever needs a part first builds it, the others wait; the
    recording thread takes over a job still queued. The settings screen switches modes live.
  - Diagnostics: `[gpu] hitch: frame N took X ms; compiled ...` for frames over 50 ms, `CRASH_COMPILE_LOG=1`
    (`compiles.csv` per frame), the `vulkan pipelines (...)` stats line, `CRASH_SHADER_COMPILE=sync|async|
    aggressive`, `CRASH_VULKAN_NO_EXT=gpl` (a driver without libraries), `CRASH_VULKAN_COLD=1` (an unused
    specialization constant per run: the driver's own cache misses, for first-run measurements).
  - The pipeline cache is written to a `.tmp` and renamed; `VK_INCOMPLETE` (workers adding) is accepted.
- Upscalers (`upscaler.cpp`) run on the renderer's device: DLSS through NGX's Vulkan API inside
  `CrashDlss.dll` (`gpu/dlss_module/crash_dlss.h`: C interface, `CRASH_DLSS_API_VERSION`; loaded at run time,
  the exe has no NGX code), FSR 3.1.4 through `amd_fidelityfx_vk.dll` (FidelityFX SDK 1.1.4; SDK 2.x is DX12
  only). The device enables the extensions they ask for (`rhi::DeviceInfo`): NGX's list, and the ones
  the FidelityFX DLL looks up by extension name (`vkGetBufferMemoryRequirements2KHR`,
  `vkSetDebugUtilsObjectNameEXT`: a null call otherwise).
- Availability (v1.05): DLSS is probed only on an NVIDIA GPU (vendor 0x10DE; `CRASH_GPU_VENDOR=amd|intel|nvidia|<hex>`
  pretends another one, `settings/gpu_vendor.h`), with both DLLs present, and NGX's
  `SuperSampling_Available` set. FSR only when the device really enabled `FSR_DEVICE_EXTENSIONS`
  (`rhi::Device::DeviceExtensionEnabled`). `upscaler::Init`: a settings.toml asking for DLSS where it can't run
  gets FSR (else off) with a log line; `xenos_renderer` puts that into the in-memory values
  (`settings::Set` + `RendererApplied`), the file only changes if the player saves. The options
  (`options.cpp`): `Change(item, values, dir, context)` skips the unavailable upscalers, `Unavailable()` greys
  "DLSS (REQUIRES NVIDIA RTX)" in both screens. The launcher asks NGX itself in a child process
  (`CrashLauncher.exe --probe-dlss <dir> <file>`, `launcher/gpu_probe.inc`, ~4 s on a thread; a driver crash
  there can't take the window).
- Upload ring (`rhi_ring.h`): a draw's uploads come before the draw is recorded, so `DrawImpl` (and
  `DrawPass`) hold them (`Device::HoldUploads`): a submission in between (ring or descriptor pool full)
  is marked where the hold began. Without it those uploads were freed and reused under the pending draw
  (`CRASH_VULKAN_RING_MB=8`: device lost in ~10 s). The ring peaks at ~47 MB in the village at 1080p.
- Safe mode / diagnostics (issue #3, black flicker on an Intel UHD): `[video] gpu_safe_mode = "on"` or a
  list of `barriers, instances, serial, memory, depth, present, cache` (`rhi::SafeMode`; `CRASH_GPU_SAFE`
  overrides), `[video] gpu_diagnostics = true` (`CRASH_GPU_DIAG`): driver, heaps, limits, extensions,
  per-format features, surface; the log also goes to `userdata\crashrecomp_log.txt`; a validation layer
  copied into `vulkan_layers\` next to the exe is loaded (`VK_ADD_LAYER_PATH`). Always logged: driver,
  heaps, limits, formats lacking a use, swap chains / VK_SUBOPTIMAL_KHR / unshown frames, ring peak and
  mid-frame submits, memory budget.
- Intel-like paths on this machine: `CRASH_VULKAN_D32=1`, `CRASH_VULKAN_NO_EXT=depth_clip,depth_bias_control,maintenance5`;
  a slow GPU: `CRASH_VULKAN_GPU_LOAD=<k>` (k thousand iterations per pixel of a 64x64 pass per frame;
  600 = ~10 ms, 1800 = ~16 ms GPU wait per frame on the RTX 5080; 2000+ with a broken ring hit a TDR).
  Don't emulate a slow GPU with a host-signalled timeline wait: present then waits on a submission whose
  wait isn't signalled yet (invalid; the NVIDIA driver hangs).
- Debug: `CRASH_GPU_DEBUG=1` (validation + synchronization validation through `VK_EXT_layer_settings`;
  `=nosync` without it, `=selftest` adds an invalid call and a write-after-write hazard to prove both
  reach the log; `run_game.py --set gpu_debug=true`),
  `CRASH_VULKAN_DEVICE=<name part>`, `CRASH_VULKAN_TRACE=N`, `CRASH_VULKAN_TRACE_PASSES=<submit>`,
  `CRASH_VULKAN_DEDICATED=1`, `CRASH_GPU_TRACE_SWAPS=N`, `CRASH_GPU_TRACE_RESOLVE=<hex>`,
  `CRASH_GPU_DUMP_RESOLVE=<hex>`, `CRASH_GPU_DUMP_PRESENT=1`. Status, parity table and the native-Linux
  plan: `runtime/NOTES.md`, "Vulkan".

## PC graphics options (`[graphics]`, `gpu/graphics_options.cpp`)

- DLSS (static `nvsdk_ngx_s.lib` in `CrashDlss.dll`) and FSR 3.1 through the FidelityFX API, upscaling only; SMAA; mipmaps +
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

## Wide screens (`gpu/widescreen.*`, `camera/hor_plus.cpp`, HUD_GS in `xenos_renderer.cpp`)

Full write-up: `runtime/NOTES.md` "Ultrawide". Settings `[video] aspect` (auto / 16:9 / 16:10 / 21:9 /
32:9), `resolution` (WxH; the width follows the aspect's list), `hud_16_9`.

- The guest keeps 16:9 at 1280x720. Host targets and resolves are `g_stretch` (= aspect / 16:9) times
  wider: every guest x goes through `XScale(scale)` (targets, resolves, clears, viewport, scissor,
  upscaler sizes and jitter, present). The scaled shader variant gets `xe_render_scale.xy` and a float2
  `xe_tex_size_scale` per fetch; it is used whenever `g_stretch != 1`.
- Camera: `pure3d::Camera::vf05` sub_823176C8 = SetFOV(fov, aspect), called every frame with the
  horizontal FOV and aspect 1; stores +20/+24 and derives +16 fov, +28 aspect (display aspect from
  `xnDisplay::vf20` sub_8238F438 = +1688/+1692), +40/+44 projection scales, +48..+76 the side planes of
  `vf16` sub_823170F8 (sphere test). `pddiBaseContext::vf29` sub_82393980 = SetProjection(near, far, fov,
  aspect) into state (context +136) +36..+48, mode at +16 (0 perspective, 1 ortho, 2 pixels);
  `vf30` sub_823939A8 = GetProjection; `xnContext::vf119` sub_8238D248 builds the matrix of the mode.
- Hor+ = SetProjection widened (same vertical FOV) + vf16 with widened planes + `Camera::vf11`
  sub_82316E58 (one frustum plane, read by the scene's sector/object culling; without it the door of
  Crash's house vanished past the old 16:9 edge) giving the widened side planes + vf119 building the
  ortho/pixel matrices from the game's own fov/aspect + GetProjection returning them.
- **Traps**: writing the widened values into the camera (+16/+28/+40/+44 or the planes) made the ice
  shards of the village blink (other code reads them). The HUD's 2D is the ortho mode of the same
  projection: built from the widened aspect, its y offset scaled with the aspect (HUD 124 px lower at
  21:9, off the screen at 32:9).
- 2D: draws into a 1280-wide 1x target (not scene, not MSAA) get `xe_hud` (b3): triangles go through
  HUD_GS (points / lines: `xe_hud_transform` in the VS). Draws of the 2D VS aecab5202246c470 whose
  triangles all get the same treatment (almost all) skip HUD_GS: `HudFoldDraw` applies its rules on
  the CPU and folds the result into the draw's c4..c7 (`CRASH_HUD_FOLD=0` turns that off; Adreno ran
  HUD_GS at 12 fps in the menus). Rectangle lists are D3D's clears and are
  never transformed (squeezed, they left the HUB's depth uncleared in strips: titan dolls cut). W = 1 triangles (the game's 2D, 640x480
  virtual through c4-c7) scale around the centre unless they span the width; full-screen ones zoom when
  they read no render target. Triangles with the same W != 1 on the three vertices are the frontend's
  sprites through a pure3d camera (W = 0.5), already placed by Hor+; big ones past the 16:9 area zoom.
  Anchored HUD (`hud_16_9 = false`): only in node 638, per draw, from the vertices of the 2D VS
  aecab5202246c470 (HudDrawSide). Bink: the decoded frame is drawn by the generic 2D shader, so videos
  are known from `cutscenes::VideoPlaying()`; scaled and the bars cleared.
- `CRASH_NO_HOR_PLUS=1` (16:9 view stretched), `CRASH_HUD_NO_GS=1` (2D per vertex), `CRASH_WINDOW_SIZE=WxH`
  (client in screen pixels, may exceed the monitor: captures), `CRASH_MONITOR=WxH` (what AUTO sees).

## Window

The present keeps the aspect ratio with black bars and the process is per-monitor DPI aware (on a high-DPI
screen the window used to be half resolution and stretched). The window opens at the picture's aspect
(720 lines at 96 DPI), fitted into the primary monitor's work area.
