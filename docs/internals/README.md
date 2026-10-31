# Internals: quick reference (EN)

Short, English cheat sheets of what was learned the hard way while building CrashRecomp: guest addresses,
hooks, traps and the reasons behind design choices. They are summaries; the full write-ups (in Portuguese)
are in [runtime/NOTES.md](../../runtime/NOTES.md), [docs/GUIA_DO_PROJETO.md](../GUIA_DO_PROJETO.md) and
[docs/dev_features.md](../dev_features.md). Each file names the NOTES section to read next.

Addresses are guest (PowerPC) addresses in the retail `default.xex` (title `565507E4`). `sub_XXXXXXXX` is the
name XenonRecomp gives the recompiled function at that address; overriding one is a strong `PPC_FUNC` in the
runtime. Before hooking a function listed here, grep `runtime/` for it: most already have a hook, and a
second `PPC_FUNC` for the same address will not link. Extend the existing one.

| File | Topic |
|---|---|
| [recompiler-and-runtime.md](recompiler-and-runtime.md) | XenonRecomp config and local patches, the kernel/XAM imports, fibers, GPU/audio basics, symbols |
| [graphics.md](graphics.md) | Vulkan renderer (rhi.h) facts, PC graphics options (DLSS/FSR/SMAA), tiling/resolves, HD texture packs, lighting remaster, window |
| [frontend-and-boot.md](frontend-and-boot.md) | The native frontend tree, direct boot (`--boot`), test events, skipping cutscenes, name entry and save deletion |
| [input-and-coop.md](input-and-coop.md) | Keyboard/mouse/DualSense, button icons, mouse in menus, local co-op player slots |
| [free-camera.md](free-camera.md) | The experimental third-person free camera, camera/collision managers |
| [performance.md](performance.md) | Where the CPU time goes, the D3D ring, optimizations done, benchmark tooling |
| [native-gpu-plan.md](native-gpu-plan.md) | The native GPU path: the Pure3D xn / Xbox D3D layers, what is done (one-pass scene, native D3D flush, shader warm-up), the road to 100% native and the task board |
| [achievements.md](achievements.md) | The Xbox 360 achievements: where the game awards them (XMsg 0xFB / 0xB0008), the SPA in the image, the store, the screen and BACK |
| [dev-tools.md](dev-tools.md) | In-game Lua, cheats/invincibility/teleport, the developer console, skins, the Lua decompiler |
| [linux_port.md](linux_port.md) | The Linux / Steam Deck port: Windows dependencies, the platform layer (`runtime/platform`, SDL3 backends), build status, x86-only code |
| [android_port.md](android_port.md) | The Android port plan: ARM64 (SIMDe/NEON, fences, FPCR, store emulation, 16 KB pages), Vulkan on Adreno/Mali, storage, touch, the APK built on the player's PC |
| [mods-and-assets.md](mods-and-assets.md) | `.rcf` overlay and the 2 GB limit, HD AI Remaster constraints, model export facts, the blue Snipe shots (package rewrite + quiver hooks) |
| [multiplayer.md](multiplayer.md) | Online multiplayer prototype: level streaming (rooms, resident set, policies tested), the 2-player limit, actor ids / transforms, the UDP mirror and bandwidth, the recommended architecture and milestones |
