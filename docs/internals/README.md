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
| [native-gpu-plan.md](native-gpu-plan.md) | Branch `native-gpu`: the Pure3D xn / Xbox D3D layers, the tiling serialization, plan and task board for a native GPU path |
| [dev-tools.md](dev-tools.md) | In-game Lua, cheats/invincibility/teleport, the developer console, skins, the Lua decompiler |
| [mods-and-assets.md](mods-and-assets.md) | `.rcf` overlay and the 2 GB limit, HD AI Remaster constraints, model export facts |
