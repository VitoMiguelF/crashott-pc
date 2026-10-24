# Native GPU path: plan (branch `native-gpu`)

> **Resumo (PT-BR).** Objetivo: deixar a renderização mais nativa e mais rápida, sem a tradução da GPU do
> Xbox 360 em tempo real, sem regressões. Medido em 2026-10-24 (base `a906172`, vila, 1080p, 5700X +
> RTX 5080): **a thread do jogo passa 62% do quadro esperando o fence do quadro anterior** e o command
> processor fica 44% ocioso: as duas threads trabalham **em série** (2,3 ms + 3,5 ms ≈ 5,8 ms por quadro,
> 162 fps). A causa é o **tiling predicado** do D3D do Xbox: entre `BeginTiling` e `EndTiling` o D3D guarda
> os comandos da cena numa lista e só os entrega à GPU no fim, uma vez por tile. A camada que o jogo usa
> para desenhar é o backend Xenon do Pure3D (`pure3d::xn*`, nomes do RTTI), que faz quase todas as
> chamadas ao D3D: 98 pontos de entrada D3D no total (`tools/d3d_map.py`). Plano: (1) shaders
> pré-compilados na instalação; (2) **cena numa passada só, sem tiling** (ainda no caminho PM4: o maior
> ganho esperado, ~1,5–2x, e acaba com os remendos de emenda de tile); (3) draws diretos: ganchos nas
> funções D3D de draw que entregam o estado ao renderer sem gerar nem interpretar PM4; (4) recursos
> nativos (texturas, buffers, render targets reais, resolves como cópias) com `--gpu-path pm4|native`;
> (5) efeitos, DLSS/FSR, iluminação e texturas HD no caminho novo; (6) limpeza e (7) desempenho. Cada
> marco tem testes, medição, commit e tarefas para o colaborador (seção "Tarefas").

Status: **Phase 0 (investigation and design)**. Nothing in the runtime has changed yet on this branch.
Owner of this file: whoever lands a milestone updates its row in [Milestones](#milestones) and the
[task board](#task-board-for-collaborators).

Related: [graphics.md](graphics.md) (renderer facts), [performance.md](performance.md) (earlier rounds),
[recompiler-and-runtime.md](recompiler-and-runtime.md), `runtime/NOTES.md` "Desempenho de CPU" and "Vulkan".

## 1. Where the time goes today (measured)

`tools/perf/bench.py native_base_sampled --profile desempenho_vila_direto --sample 300 --sample-start 20
--sample-seconds 15`, worktree build of `a906172` (Vulkan, llvm-mingw), 1080p native, no frame limit, window
focused, alone on the machine. Result: **162 fps** (6.17 ms, p99 8.3 ms); renderer pacing 5.83 ms/frame.

| Thread | Busy (share of one core) | Breakdown |
|---|---|---|
| Game main thread | 67.5% | **62.2% in D3D's fence wait** (`sub_8226D968` < `sub_8226C958` < `sub_8226CCC8`, the wait for the previous frame's swap fence, `device+14788`); 28.0% game/engine code; 5.3% Xbox D3D (building PM4); 4.5% `pure3d::xn*` |
| GPU command processor | 53.6% | 3.5 ms/frame of work: record 0.99, state/shaders/indices 0.50, vertex upload 0.49, textures 0.37, resolve 0.37, constant buffers 0.30, index upload 0.18, pipelines 0.14; **~44% idle waiting for commands** |
| vblank/interrupt thread | 71% | 92% of it is `[system]` time in timed condition-variable waits (1000 Hz vblank with no limit). Burns a core but is not on the critical path |

Per frame (village): 1,603 draws, 1.38 M indices, 2,027 texture lookups, 20 textures hashed (0.7 MB), 5 texture
uploads, 1.71 MB of PM4, ~80 indirect buffers, ~2,300 predicated packets skipped.

**Reading.** Game work (2.3 ms) + command-processor work (3.5 ms) = 5.8 ms ≈ the frame time: the two threads
almost never overlap. The game waits for the CP to finish frame N before it can fill the first segment of
frame N+1 (D3D allows one frame in flight; we keep that, the game reuses dynamic buffers), and the CP waits for
the game because **the scene's commands only reach it at `EndTiling`** (section 3.3). The GPU itself waits ~0
(DLSS or a lower resolution change nothing, as in the earlier rounds).

So the order of the wins is (M2 confirmed the first one: 2.4x):
1. Let the CP (or its replacement) work **while** the game builds the frame: remove the tiling deferral.
   Theoretical ceiling ≈ max(2.3, 3.5) ms instead of the sum (≈ 1.6x), plus less CP work because each scene
   draw is processed once instead of once per tile.
2. Then shrink the CP work itself (no PM4 encode on the game thread, no PM4 parse, no EDRAM emulation,
   fewer barriers/render passes): that is the native path.
3. Shader/texture translation at run time is **not** a per-frame cost any more (shaders are cached by hash,
   texture hashing is ~0.7 MB/frame thanks to the write watch). Precompiling them removes first-use hitches
   (~2 s the first time a resolution is used), not average fps.

## 2. The layers

```
game code (CRenderManager, CRenderObject3D, ...)
  -> Pure3D scene graph (pure3d::CompositeDrawable, PrimGroupOptimized, Geometry, ...)
    -> pddi = Pure3D Device Driver Interface: pddiRenderContext / pddiDisplay / pddiTexture / pddiPrimBuffer / pddiShader
      -> Xenon backend: pure3d::xnContext, xnDisplay, xnDevice, xnTexture, xnPrimBuffer(Stream),
         xnShader + 15 effect shaders (xnSimpleShader, xnReflect/Refract, xnWater*, xnDOF, xnMotionBlur,
         xnShadow, xnParticle, xnBink, xnHeatShimmer, xnColourMatrix, xnRTRestore, xnError)
        -> Xbox D3D (statically linked, no symbols): 0x82268C00-0x822845A0 + 4 draw functions at 0x823EA9A8-0x823EBBD8
          -> PM4 in the D3D ring (32 MB, gpu/ring_buffer_hook.cpp), kicked through CP_RB_WPTR (MMIO trap)
            -> runtime: command_processor.cpp (PM4 parse, register file) -> xenos_renderer.cpp (state ->
               draws, EDRAM render targets, resolves, tile stash, textures, temporal passes, present) -> rhi_vulkan.cpp
```

Everything below "Xbox D3D" is what the runtime emulates today. The xn layer (0x8238B000-0x823AC000) is the
only big D3D client; the rest are listed in 2.2.

### 2.1 Key functions (this dump, title 565507E4)

Names in *italics* are inferred from behaviour (call sites, packets written, Xbox D3D API shape); confirm
before relying on them. `tools/d3d_map.py` prints the full list with callers.

| Address | What | Evidence |
|---|---|---|
| `pure3d::xnContext::vf04` 0x8238CD00 | *BeginFrame / begin scene*: calls D3D *BeginTiling* 0x8227A5F8 | only caller with xn vf101 |
| `pure3d::xnContext::vf05` 0x8238E068 | *EndFrame / end scene*: *EndTiling* 0x8227AB80 + *Resolve* 0x8227F9E8 | |
| 0x8238CE98 (xn, non-virtual) | mid-frame EndTiling + Resolve (called from `CRenderManager::vf10` 0x82225AD0): the scene resolve to 0x0A642000 before refraction | callers |
| `xnContext::vf101` / `vf102` 0x8238CBD0 / 0x8238CBE0 | second tiling bracket: 0x8227B038 / 0x8227B380 (-> 0x8227B2B8 -> EndTiling core) | |
| `xnContext::vf34` 0x8238DEC8 -> `xnPrimBuffer::vf15` 0x82398FD8 | draw a prim buffer: 0x82272A08, 0x82272B28, 0x822762D0 (*SetStreamSource / SetIndices / vertex declaration*), then the D3D draw 0x823EB398 / 0x823EB780 | 65% of the game thread is inclusive under it (waits included) |
| 0x823EA9A8, 0x823EAE48, 0x823EB398, 0x823EB780 | D3D *Draw[Indexed]Vertices* variants: flush dirty state (0x8226DF70, 0x8226E308-0x8226E6A8, 0x8226F448 = shader load + constants) and write the draw | |
| `xnDisplay::vf04` 0x8238F5B0 | *InitDisplay*: creates the device (0x8226D300 reads D3DRING_BUFFER_PARAMETERS) and the render target surfaces / tile layout | |
| `xnDisplay::vf15` 0x8238EFE8 -> 0x82274080 | *Swap* (writes the frame fence to `device+14788`) | performance.md |
| effect shaders `vf14` | *SetVertexShader / SetPixelShader / Set*ShaderConstantF* family: 0x82275C80, 0x82275D88, 0x82275DE8, 0x82275E48, 0x82276100; textures/samplers 0x8226A990 | every effect calls 0x82275E48 + 0x82276100 |
| `xnContext::vf56..vf97` | ~25 tiny D3D setters 0x82270610-0x82271030, 0x822712C8 (*SetRenderState_\**) | 5-14 instructions each |
| `xnTexture::vf11/13/14` | texture create / lock / unlock: 0x82268CD0, 0x8226A660-0x8226A740 | |
| 0x8226CCC8 / 0x8226CE40 | D3D segment switch / kick | performance.md |
| 0x8226C958 / 0x8226D968 | wait for a fence / one spin step (hooked in `ring_buffer_hook.cpp`) | |
| 0x82281D00 / 0x82281E70 | tiling core: start deferring (sets `device+13088`), replay the deferred list (`device+13224`) | 3.3 |

Device fields seen so far: `+10768` pointer to the GPU-written fence, `+10780` latest issued fence,
`+10812/10813` flags, `+13088` tiling active, `+13224..13232` deferred indirect-buffer list, `+14760` segment
size, `+14776` current segment, `+14788` swap fence, `+16576` primary ring.

### 2.2 D3D callers outside the xn layer

From `tools/d3d_map.py` (only these must be covered besides xn when D3D is replaced):
- `movie::PlayerBink` (0x822D9C70, 0x822D9CB8): Bink frame textures (the draws go through `xnBinkShader`).
- `CFrontendManager` (0x821D91F0, 0x821DCC90) -> 0x82284350 / 0x822843B8 (end of the range; may be a query or
  not D3D at all: check).
- XGraphics-style helpers 0x823EBBD8-0x823EC880 (texture header / layout helpers called by the game's loaders).
- Two internal ones (0x82268BB0, 0x823E8EC0 -> 0x82268C18).

Pure3D's own loaders (`pure3d::TextureBuilder`, `ImageConverter`, `ShaderLoader`...) talk to `xnTexture` /
`xnShader`, not to D3D, so they stay as they are.

## 3. Decisions

### 3.1 Interception level

| Level | What we would replace | Pros | Cons |
|---|---|---|---|
| PM4 (today) | nothing | complete, already at parity | parse cost, EDRAM emulation, tiling deferral, serialization |
| **D3D API** (Unleashed Recompiled's "video" layer idea) | the 98 D3D entry points the game calls | bounded and mechanical; one implementation covers every xn effect, Bink, the frontend; game and Pure3D code untouched | stripped library: each entry point must be identified; D3D objects (textures, buffers, shaders) are Xenos-format structures in guest memory |
| pddi (xn backend) | `xnContext`/`xnDisplay`/`xnTexture`/`xnPrimBuffer` + 16 shader classes | the highest level (matrices, materials, prim buffers) | re-implements every effect's logic: more code and more risk of visual divergence |

**Decision: the D3D API level**, with two exceptions where the xn level is simpler: the frame/tiling
bracket (`xnContext::vf04`/`vf05`, `vf101`/`vf102`, 0x8238CE98) and `xnDisplay::vf04` (render target
setup). Hooks are strong `PPC_FUNC(sub_XXXXXXXX)` overrides that call `__imp__sub_XXXXXXXX` (the original)
when `gpu_path = pm4`, so the PM4 path stays selectable at run time until the native path is at parity.

How Unleashed Recompiled does it (ideas only; **do not copy its code**: check its license first, ours is
MIT): it overrides the game's D3D-level functions, keeps host-side objects behind the guest D3D handles,
recompiles the Xenos shaders ahead of time (XenosRecomp) and renders in one pass with real render targets,
turning resolves into copies. Our case is easier in one way (one Pure3D backend makes almost every call) and
harder in another (no symbols: we identify the functions by behaviour).

### 3.2 Reuse the renderer core

`xenos_renderer.cpp` already turns *Xenos register state* into Vulkan draws (`renderer::Draw(DrawPacket)` reads
the register file). It owns everything the PC features hook into: scene vs output scale, the velocity and
camera capture for DLSS/FSR, the lighting pass, HD texture keys, shader patches by hash, SMAA, overlay,
screenshots, present. The native path keeps it as the backend and changes **what feeds it**:

- M3 ("direct draws"): at each D3D draw the hook builds the same register/constant/fetch state from the D3D
  device's shadow state (the D3D keeps the Xenos register values it will write, in dirty groups) and hands
  it over without PM4. Shader microcode is passed by handle (`LoadShaderHandle`), not re-hashed.
- M4 (native resources): textures, vertex/index buffers, shaders and render targets become host objects
  created when the game creates them; the renderer's per-draw lookups (`GetTextureView`, vertex range
  copies, `GetRenderTarget`) become handle dereferences.

The work moves from "CP thread parses PM4" to "a render thread consumes a compact host command stream"
(one frame in flight, same fence semantics: `*(device+10768)` advances when the render thread has consumed
the frame, so the game's buffer reuse stays safe).

### 3.3 Predicated tiling and EDRAM

What D3D does: `BeginTiling` (0x8227A5F8 -> 0x82281D00) sets `device+13088`; from then on the kick
(0x8226CE40) appends each full segment as an indirect-buffer packet to the list at `device+13224` instead of
the primary ring. `EndTiling` (0x8227AB80 -> 0x82281E70) replays the list once per tile (3 tiles, 448 px wide,
overlapping 32: scissor 0-448 / 416-864 / 832-1280) under `SET_BIN_SELECT` predication, resolving each tile.
Consequences today: the CP gets the scene late (serialization, section 1), parses it three times, and the
tile seams need the `TileStashBeforeResolve` workaround (ice band at x = 832).

Done in M2 (`runtime/gpu/tiling_hooks.cpp`, on by default, `CRASH_SINGLE_PASS=0` = the console's tiling):
- The scene targets were tile-sized host images (480x720: `xnDisplay::vf04` creates color 0, depth/stencil
  and color 1 = velocity through 0x8226A868 with width align32(1280/3 + 63)), drawn once per tile. They are
  now created at 1280x720 with explicit EDRAM bases 1440 / 2048 / 2880 in `D3DSURFACE_PARAMETERS` (Base,
  HierarchicalZBase, ColorExpBias): three full-size 2x surfaces overflow D3D's 2048-tile allocator (the 2nd and
  3rd creations returned null and `Resolve` crashed reading RenderTarget[1]). Render targets are keyed by
  (base, host width, format) without the MSAA, so the bases must also differ from the 1x post/HUD targets
  (color at 0, depth at 720).
- `BeginTiling` is skipped (no clear is asked for), so nothing is deferred. An `EndTiling` (xn vf05 and the
  mid-frame 0x8238CE98) becomes one `Resolve` of the whole surface with its flags/clear values/parameters.
- `xnContext+1220` stays set: the xn helpers keep their per-tile resolves (`SetPredication(2 << tile)` +
  `Resolve(tile rect, point = rect origin)`); the first becomes a whole-surface resolve and the other two are
  dropped. The xn code's non-tiled branches (+1220 = 0) are dead code here: they read depth/shadow texture
  slots that are never created (and mix up indices 295/297).
- `TileStashBeforeResolve` is off in this mode (nothing to stitch).
- EDRAM: render targets are host images keyed by (EDRAM base, pitch, format, MSAA) today. On the native path
  they are keyed by the guest surface object (`D3DSurface`) instead; EDRAM aliasing tricks the game relies on
  must be kept explicitly: the depth-only clears into the velocity RT (`DEPTH_ALIAS_PS`, stencil ref 0x80 =
  neutral velocity), the 640-pitch 4x fast clear of the 1x depth, depth resolves to packed RGBA8 (stencil in
  R) for the stencil shadows. Each one is a named case in the native code, with its NOTES reference.
- Resolves become copies/blits (MSAA resolve, format conversion, exp bias, swap), to the same host textures
  the renderer's resolve targets use now (so the scene/velocity/depth resolves that DLSS/FSR and the lighting
  pass consume stay where they are).

### 3.4 Shaders

- Today: microcode -> HLSL (`shader_translator.cpp`) -> DXC -> SPIR-V at first use, cached by the FNV-1a of the
  microcode (`userdata\shadercache\<hash>.spv` + `vulkan_pipelines.bin`); graphics patches by hash
  (`graphics::PatchShader`: motion blur, DOF, shadow softness, mipmap gradients) and `shaderoverride\`.
- The game's shaders are the containers in `<game>\shaders\*.out` (38 of them; the `.updb` next to them carry
  the original HLSL, useful to read but not needed). `--test-shaders <dir>` already translates them all.
- Offline plan (M1): at install (`tools/setup.py`, `CrashSetup.exe`) and at first run if missing, translate and
  compile every `.out` plus the variants the renderer derives (scaled/patched/MSAA variants: enumerate them
  from a "variants seen" list the runtime appends to, `userdata\shadercache\variants.txt`), and warm the
  pipeline cache. Unseen shaders keep the current runtime path as the fallback. Shader caches are game-derived
  and never go into git or releases (generated on the player's machine).

### 3.5 Textures

- Today: decode/untile/byte-swap at first use, re-checked per frame by hash, skipped while the pages are
  write-protected and clean (`texture_watch`), HD replacement keyed by the hash of the decoded base level.
- Native: decode when the game creates/unlocks the texture (xnTexture vf11/13/14, Bink frames), keep the host
  image behind the guest D3D texture; the write watch stays as the safety net for textures the game writes
  directly. A persistent on-disk decode cache is **low priority** (5 uploads/frame, 0.37 ms/frame today); it
  would only shorten level loads. HD keys stay the same function of the decoded base level, so existing packs
  keep working.

## 4. Keeping every PC feature working

| Feature | Fed today by | On the native path |
|---|---|---|
| DLSS / FSR (motion vectors, camera) | scene VS constants `aecab5202246c470` / `429975a046a1a236` (c4-c7 MVP, c16-c19 previous MVP), velocity RT, `NoteSceneResolve`, `UpscaleBeforeReading` | same: constants still reach the renderer per draw; the resolves feeding it are the same host textures |
| Scene vs output scale | RTs first used with 2x/4x MSAA = scene | same rule, applied to the surface object at creation |
| HUD split (upscaler before the first 1x draw sampling the scene resolve) | `UpscaleBeforeReading` | unchanged (texture binding is still seen per draw) |
| Lighting remaster | `RunTemporal` / `ScenePassActive` in the scene pass | unchanged entry point; with one pass there is one scene pass per frame |
| HD texture packs | key = hash of decoded base level at upload | computed at texture creation/unlock instead of first use: same key |
| Shader patches by hash | `graphics::PatchShader` in `PrepareShader` | unchanged (same microcode hash) |
| Screenshots, overlay (F1), subtitles, settings UI, console, icons | `Swap` and the passes after it | unchanged (`renderer::Swap`) |
| Palettized button icons (k_8 + palette at s8) | `FindIconTexture` per draw | unchanged |
| SMAA / FXAA, mipmaps/aniso, frame limiter, vsync | renderer / `PaceFrame` in `VdSwap` | unchanged; the frame limiter moves to the native Swap hook |
| Free camera, dev console, skins | game-side hooks | unaffected |

Rule for every milestone: if a feature is fed by something the milestone removes, the milestone moves the
feed, and its test profile is in the milestone's checklist.

## 5. Milestones

Every milestone: (1) lands behind a switch, default off until parity; (2) compares captures against the PM4
path (`menu`, `vila_sem_cena`, `iluminacao_tela`, `configuracoes`; captures in `build\native_gpu\<milestone>\`);
(3) measures `desempenho_vila_direto` with `tools\perf\bench.py` (alone on the machine); (4) is committed on
`native-gpu` with the profiles named in the message; (5) updates this table and the task board.

Switch: `[video] gpu_path = "pm4" | "native"` in settings, `CRASH_GPU_PATH`, `--gpu-path`; finer switches per
milestone while it is experimental (`CRASH_NATIVE_TILING=0`, ...). The user's suggested order (a-g) maps as
noted; tiling moves up from (f) to M2 because it is the measured bottleneck and does not need the rest.

| # | Milestone | Maps to | Expected gain | Status |
|---|---|---|---|---|
| M0 | This plan, `tools/d3d_map.py`, baseline measurement | - | - | done (2026-10-24) |
| M1 | Shaders precompiled at install/first run; variants list; pipeline cache warm-up | (a) | no first-use hitches | |
| M2 | Scene in one pass (no predicated tiling), still on PM4 | (f) | measured **179 -> 429 fps** in the village (2.4x), CP 3.3 -> 1.4 ms/frame, draws 1,585 -> 574/frame; tile stash off | done (2026-10-24), default on, `CRASH_SINGLE_PASS=0` = tiling |
| M3 | Direct draws: D3D draw hooks feed the renderer from the device shadow state, a host command stream to the render thread; PM4 only for what is not hooked yet | (b) | M3a (native flush + kick, identical PM4): 441 -> 464 fps; the transport change (M3b) no longer pays, see M3 status | M3a done (2026-10-24) |
| M4 | Native resources: textures/buffers/shaders/render targets as host objects; resolves as copies; frontend first, then the village | (b), (c), (d) | per-draw lookups and EDRAM bookkeeping gone | |
| M5 | Post effects parity: motion blur, DOF, refraction/reflection, water, heat shimmer, Bink | (d) | - | |
| M6 | DLSS/FSR, lighting remaster, HD packs verified on native | (e) | - | |
| M7 | `native` default; PM4-only workarounds removed (tile stash, depth-alias special cases that no longer apply) | (f) | simpler code | |
| M8 | Performance: fewer render passes/barriers (~300/~650 per frame now), multi-threaded recording, state sorting | (g) | GPU-side and record time | |

### M1 details (shaders)
- `renderer::PrecompileShaders(folder)` = `TestShaders` that also writes the pipeline cache; `--precompile-shaders`
  and a launcher/installer step; skip when `shadercache\manifest.txt` matches the exe build id + DXC version.
- The runtime appends every (hash, variant) it compiles at run time to `variants.txt`, so the next precompile
  covers it; ship no game-derived list.
- Test: delete a *copy* of the test shader cache, precompile, run `vila_sem_cena` and check the log has no
  "compiling" lines after boot; first-frame hitch gone in `desempenho_vila_direto` (max frame time).

### M2 details (one pass) — done, see 3.3
- Trace (`CRASH_TRACE_TILING=<n>[@<frame>]`, village frame): one bracket per frame. `BeginTiling(flags 0, 3
  tiles, no clear)`, `Clear(0x1F)` + `Clear(0x2, 0x808080)` (velocity), the scene, 10 logical resolves × 3
  tiles (scene copies to the refraction texture, depth resolves with flags 0x14), two stencil clears over the 3
  rects, `EndTiling(0x300 = resolve + clear color and depth, to one of the two front-buffer textures)`, then a
  `Clear(0x30)` over the 3 rects outside the bracket. `xnContext::vf101/vf102` (0x8227B038 / 0x8227B380) are
  per-draw calls, not tiling.
- Known leftovers: the xn code still issues 3 SetPredication calls per logical resolve (cheap); the mid-frame
  EndTiling path (0x8238CE98, `CRenderManager::vf10`) is handled by the same hook but did not occur in the
  village; check other levels when they are profiled.

### M3 status (2026-10-24)
- Sampled after M2, the D3D draw functions were ~15% of the game thread: ~5% the residual fence wait and the
  rest the recompiled flush of dirty groups (0x8226E308 3.4%, 0x8226E6A8 2.8%) and the kick (0x8226C3F0 3.3%,
  the CP_RB_WPTR store through the MMIO page trap). Setters called by xn are cheap.
- **M3a** (`runtime/gpu/d3d_native.cpp`, on by default, `CRASH_NATIVE_D3D=0` = the recompiled originals): native
  `8226E308` / `8226E6A8` (block copies of the big-endian shadow registers; the cross-segment case still goes
  through D3D's 0x8226DCD0) and a native kick (writes the INDIRECT_BUFFER packets, hands CP_RB_WPTR to the
  command processor without the trap; the PIX/capture paths stay with the original). Output is byte-identical:
  `CRASH_NATIVE_D3D_VERIFY=1` runs both and compares (vila_sem_cena: 2.1 M register and 0.5 M constant flushes,
  0 mismatches). Village: 441 -> **464 fps** (both with the tile-resolve dedupe, which only became active
  here: the M2 Resolve hook had been declared `extern "C"` and the game never called it).
- What limits now (sampled): game thread 62% game/engine, 13% xn, 5.6% D3D, **19% waiting for the previous
  frame's fence**; the CP is ~50% idle but is slower than the game emits during the render phase (~1.25 ms of
  CP work per frame: `DrawImpl` 31% inclusive, Vulkan draw/rendering/present ~17%), and D3D's 1-frame-in-flight
  lock (required: the game reuses dynamic buffers) makes the game wait at the next frame. Smaller D3D segments
  (64/32/16 KB, now cheap without the trap) did not help (450-457 fps vs 464).
- **M3b** (a host draw record instead of PM4 for the draw state) would remove only the CP's PM4 parse of that
  state (ExecuteBuffer + WriteRegisters, ~5% of the CP), not the renderer work: postponed in favour of the CP's
  renderer cost (M8: `DrawImpl`, texture lookups, fewer render passes/barriers) and M4 (native resources, which
  remove per-draw texture/vertex lookups), which now decide the frame time together with the game code.

### M3/M4 details
- Identify and name each of the 98 entry points (task board T3): signature, which device fields it reads and
  writes, which PM4 it emits. Record them in `runtime/gpu/native/d3d_functions.h` (address, name, notes).
- `runtime/gpu/native/`: `d3d_hooks.cpp` (the `PPC_FUNC` overrides, one file per group if it grows),
  `d3d_device.*` (shadow-state reader), `command_stream.*` (game thread -> render thread), `resources.*`.
- Start with the frontend (menu, settings: few draws, no tiling, Bink), then the village.

## 6. How to test

- Captures: run the same profile with `--set env={ CRASH_GPU_PATH = "native" }` and without; compare with
  `tools\perf\side_by_side.py` / `compare_runs.py` (mean difference per channel; the earlier rounds accepted
  ≤ 3/255 on the first 20 playable seconds). Copy the chosen PNGs to `build\native_gpu\<milestone>\`.
- Profiles: `menu`, `configuracoes` (frontend + settings UI + overlay), `vila_sem_cena` (village, motion blur,
  shadows, resolves), `iluminacao_tela` (lighting), `fase_direta` (a level with refraction/ice), plus
  `gpu_debug=true` once per milestone (validation clean).
- Performance: `python tools\perf\bench.py <name> --profile desempenho_vila_direto` (add `--sample 300
  --sample-start 20 --sample-seconds 15` for a profile; `tools\perf\profile.py` names the samples).
  Wait until no `CrashRecomp.exe` runs. Never use the player's userdata.
- Baseline (M0): 162 fps, 6.17 ms, p99 8.3 ms, CP 3.5 ms/frame (`build\desempenho\native_base_sampled.json`).
- After M2 (same machine, window unfocused in both): tiling 179 fps / single pass **429 fps** (p99 3.3 ms, 1% low
  214 fps), CP 1.39 ms/frame (`m2_final_tiled.json`, `m2_final_single.json`). Sampled: the game thread waits on
  the fence 5% of the time (was 62%); 63% game/engine, 16% D3D, 15% xn; the CP thread is idle most of the time.

## 7. Task board (for collaborators)

Pick a task, put your name and branch next to it (on `native-gpu`, or a branch off it), and keep to the files
listed so two people never edit the same file.

| Task | Files | Needs | Done when |
|---|---|---|---|
| T1 Shader precompile (M1) | `gpu/xenos_renderer.cpp` (`TestShaders` area only), `gpu/rhi_vulkan.cpp` (pipeline cache), `main.cpp` (flag), `tools/setup.py` | nothing | M1 test above passes |
| T2 Tiling bracket trace (M2 step 1) | new `gpu/tiling_hooks.cpp` (logging only) | nothing | a table of brackets per frame in NOTES |
| T3 Name the D3D entry points | new `gpu/native/d3d_functions.h`, this doc 2.1 | `tools/d3d_map.py`, Xbox 360 D3D API knowledge | every entry point has a name + confidence |
| T4 Device shadow-state layout | new `gpu/native/d3d_device.h` | T3 for the draw/flush functions | the renderer state of one draw can be rebuilt from guest memory and matches the PM4 path (logged side by side) |
| T5 One-pass scene (M2) | `gpu/tiling_hooks.cpp`, `gpu/xenos_renderer.cpp` (`Resolve`, tile stash) | T2 | M2 checks pass, bench recorded |
| T6 Capture comparison wrapper | new `tools/native_gpu_compare.py` (wraps `run_game.py` + `tools/perf/compare_runs.py`) | nothing | one command runs a profile on both paths and prints the per-shot difference |

## 8. Risks and open questions

- **Inlined D3D**: some D3D state setters may be inlined into xn code (direct stores to device fields with no
  call). T4 must scan the xn range for stores through the device pointer; anything inlined has to be read from
  the shadow state rather than hooked.
- **One frame in flight** stays: the game reuses dynamic vertex buffers on that guarantee. The native fence
  must advance only after the render thread has read the frame's guest data.
- **EDRAM aliasing** cases (3.3) are the likeliest source of visual regressions; each needs a test shot.
- **Tile-sized surfaces**: if the game sizes some surface per tile (pitch 448-ish) the one-pass path must
  allocate it full size; M2 step 1 tells.
- The gain of M2 depends on the game's frame shape (how much of its 2.3 ms comes before the first draw); the
  bench answers it, the 1.6x ceiling is an estimate.
- Debuggability: keep `CRASH_GPU_PATH=pm4` working until M7 so any regression can be bisected by path.
