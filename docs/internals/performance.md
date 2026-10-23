# Performance

Full write-up: `runtime/NOTES.md` "Desempenho de CPU" (two rounds); GUIA "Desempenho". Tools: `tools/perf/`.

## Where the time goes

- Round 1 finding: CPU bound on the **GPU command-processor thread** (PM4 -> D3D12 translation in
  `runtime/gpu`), not the game thread. The game spins in D3D's fence wait (sub_8226C958 / spin sub_8226D968).
  DLSS or a lower resolution changes nothing (GPU wait ~0).
- After round 2 the **game thread** is the bottleneck (vertex transform sub_82353478 ~18%, D3D spin ~20%) and
  the CP thread is mostly idle.
- The heaviest scene is the L1_E1 village (NEW GAME): ~1,600 draws, 1.35 M indices, ~100 textures / 25 MB
  hashed per frame.
- The title's D3D ring = 6 MB in 32 segments of 192 KB (`D3DRING_BUFFER_PARAMETERS` at
  `D3DPRESENT_PARAMETERS+72`, sub_8226D300). Shader constants live in it (~6 MB/frame in the village), so it
  filled. Now 32 MB (`gpu/ring_buffer_hook.cpp`, `CRASH_D3D_RING_MB`).
- D3D throttles to 1 frame in flight: Swap (sub_82274080) stores its fence in device+14788 and the next segment
  switch (0x8226CE10) waits on it. **Don't lift that**: the game reuses dynamic buffers.

## Done (no visual or gameplay change)

- Round 1: single-pass BuildIndices, bulk register copies in PM4, SSE texture content hash, per-endian vertex
  copy, lazy fence retire, bigger D3D ring, D3D12 state cache between draws (`g_drawState`, invalidated in
  Submit / Resolve / RunTemporal / Swap), tearing flag. Village walk: 42 -> ~99 fps (Ryzen 7 5700X).
- Round 2: texture write watch (`gpu/texture_watch.*`: protect stable textures' pages in both physical views;
  a VEH unprotects 64 KB and marks them dirty; kernel writes already go through bounce buffers) was the big
  one (123 -> 169 fps). Power-throttling opt-out + ABOVE_NORMAL threads fixed unfocused-window fps. ~94 -> 176
  fps overall.
- Recompiler flags that are safe vs. that break the game: see
  [recompiler-and-runtime.md](recompiler-and-runtime.md).

## Benchmarking

- Profile `desempenho_vila_direto` (boot `level:L1_E1` + `skip_cinematics`, 20 s still after
  `gameplay_ready`, ~45 s per run) with `tools\perf\bench.py ... --profile desempenho_vila_direto`.
- `desempenho_vila` goes through the menus with `CRASH_MENU_AUTOPILOT=newgame` (`settings/menu_autopilot.cpp`)
  and `CRASH_FPS_LIMIT_AT`; no button skips the opening cutscene there, so prefer the direct one.
- `tools/perf/`: `sampler.cpp` (SuspendThread + StackWalk64), `profile.py` (names from `build\CrashRecomp.map`
  + `functions.csv`), `system_callers.py` / `hot_lines.py` (source lines from the PDB; the runtime builds with
  `-gline-tables-only`), `bench.py`, `walk_window.py`.
- **Benchmark only with no other CrashRecomp.exe running**: concurrent instances produce fake hitches. Keep
  runs few and targeted.
