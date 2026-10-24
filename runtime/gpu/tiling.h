#pragma once

// Single-pass scene instead of the console's predicated tiling (tiling_hooks.cpp,
// docs/internals/native-gpu-plan.md M2). On unless CRASH_SINGLE_PASS=0. Any thread.

namespace gpu
{
    bool SinglePassScene();
}
