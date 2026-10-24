// Native versions of the title's hottest Xbox D3D functions (docs/internals/native-gpu-plan.md, M3).
//
// After the single-pass scene (tiling_hooks.cpp) the game thread is the limit, and ~15% of it is
// D3D's draw path, sampled: the recompiled flush of dirty state groups (0x8226E308, 0x8226E6A8: one
// big-endian load + store per dword) and the kick (0x8226C3F0), whose store to CP_RB_WPTR goes
// through the MMIO page trap (an access violation, VirtualProtect, a single step, VirtualProtect).
// These replacements keep D3D's state and output exactly as they were (same PM4 in the same ring;
// CRASH_NATIVE_D3D_VERIFY=1 runs the original too and compares the bytes) and do the work natively:
// block copies of the shadow registers, and the write pointer handed to the command processor
// directly.
//
// D3D state model (sub_823EB780, the indexed draw): five 64-bit dirty masks at device+0..+32; each bit
// is a group of shadow registers stored in the device in Xenos register order (VS constants at +1792
// = register 0x4000, PS constants at +5888 = 0x4400, context registers at +10240 = 0x2000, ...).
// At a draw, D3D writes each run of dirty groups as a type-0 packet into the current segment
// (cursor = last written dword at device+48, end at device+52) and clears the masks.
//
// CRASH_NATIVE_D3D=0 turns all of this off.

#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "gpu.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_8226E308);  // flush register groups: one register per bit
extern "C" PPC_FUNC(__imp__sub_8226E6A8);  // flush shader constants: 16 dwords (4 float4) per bit
extern "C" PPC_FUNC(__imp__sub_8226C3F0);  // kick: indirect buffers into the primary ring + CP_RB_WPTR
PPC_FUNC(sub_8226DCD0);                    // write a run that crosses the segment end (switches segment)
PPC_FUNC(sub_8226BD58);                    // wait for room in the primary ring

namespace
{
    constexpr uint32_t DEVICE_CURSOR = 48;      // last dword written in the current segment
    constexpr uint32_t DEVICE_SEGMENT_END = 52;

    bool Enabled()
    {
        static const bool on = [] {
            const char* text = getenv("CRASH_NATIVE_D3D");
            bool enabled = !(text && strcmp(text, "0") == 0);
            Log("[d3d] native draw-state flush and kick: %s", enabled ? "on" : "off (CRASH_NATIVE_D3D=0)");
            return enabled;
        }();
        return on;
    }

    bool Verify()
    {
        static const bool on = getenv("CRASH_NATIVE_D3D_VERIFY") != nullptr;
        return on;
    }

    uint32_t CountLeadingZeros(uint64_t value)
    {
        return value ? uint32_t(__builtin_clzll(value)) : 64u;
    }

    uint64_t ShiftLeft(uint64_t value, uint32_t count)
    {
        return count >= 64 ? 0 : value << count;
    }

    // Guest-to-guest copy of big-endian dwords: the bytes don't change.
    void CopyDwords(uint32_t to, uint32_t from, uint32_t count)
    {
        memcpy(g_memory.Translate(to), g_memory.Translate(from), size_t(count) * 4);
    }

    // D3D's own continuation for a run that doesn't fit the segment: writes what fits, switches to
    // the next segment (which may wait for the GPU) and goes on. Returns the new cursor.
    uint32_t WriteAcrossSegment(PPCContext& ctx, uint8_t* base, uint32_t device, uint32_t cursor, uint32_t reg, uint32_t source,
                                uint32_t count, uint32_t granularity)
    {
        ctx.r3.u64 = device;
        ctx.r4.u64 = cursor;
        ctx.r5.u64 = reg;
        ctx.r6.u64 = source;
        ctx.r7.u64 = count;
        ctx.r8.u64 = granularity;
        sub_8226DCD0(ctx, base);
        return ctx.r3.u32;
    }

    // sub_8226E308(device r3, mask r4, first register r5, shadow r6): every set bit (most significant
    // first) is one register; each run of set bits becomes one type-0 packet.
    void FlushRegisters(PPCContext& ctx, uint8_t* base)
    {
        uint32_t device = ctx.r3.u32, reg = ctx.r5.u32;
        uint64_t mask = ctx.r4.u64;
        uint32_t source = ctx.r6.u32 - 4;  // points at the dword before the next one to copy
        uint32_t cursor = LoadBE32(device + DEVICE_CURSOR);
        while (mask)
        {
            uint32_t skip = CountLeadingZeros(mask);
            reg += skip;
            source += skip * 4;
            mask = ShiftLeft(mask, skip);
            uint32_t run = CountLeadingZeros(~mask);
            if (cursor + run * 4 >= LoadBE32(device + DEVICE_SEGMENT_END))
            {
                cursor = WriteAcrossSegment(ctx, base, device, cursor, reg, source, run, 1);
            }
            else
            {
                StoreBE32(cursor + 4, ((run - 1) << 16) | reg);
                CopyDwords(cursor + 8, source + 4, run);
                cursor += 4 + run * 4;
            }
            source += run * 4;
            reg += run;
            mask = ShiftLeft(mask, run);
        }
        StoreBE32(device + DEVICE_CURSOR, cursor);
    }

    // sub_8226E6A8(device r3, mask r4, first register r5, shadow r6): every set bit is 16 constant
    // registers (4 float4). The data of a packet is 16-byte aligned (VMX copies): type-2 NOPs pad
    // the header to an address = 12 mod 16.
    void FlushConstants(PPCContext& ctx, uint8_t* base)
    {
        uint32_t device = ctx.r3.u32, reg = ctx.r5.u32;
        uint64_t mask = ctx.r4.u64;
        uint32_t source = ctx.r6.u32 - 4;
        uint32_t cursor = LoadBE32(device + DEVICE_CURSOR);
        while (mask)
        {
            uint32_t skip = CountLeadingZeros(mask);
            reg += skip * 16;
            source += skip * 64;
            mask = ShiftLeft(mask, skip);
            uint32_t run = CountLeadingZeros(~mask);
            uint32_t count = run * 16;
            if (cursor + (count + 3) * 4 >= LoadBE32(device + DEVICE_SEGMENT_END))
            {
                cursor = WriteAcrossSegment(ctx, base, device, cursor, reg, source, count, 16);
            }
            else
            {
                uint32_t pad = (cursor & 15) == 4 ? 1 : (cursor & 15) == 0 ? 2 : (cursor & 15) == 12 ? 3 : 0;
                for (uint32_t i = 0; i < pad; i++)
                {
                    cursor += 4;
                    StoreBE32(cursor, 0x80000000);
                }
                StoreBE32(cursor + 4, ((count - 1) << 16) | reg);
                // lvx/stvx ignore the low 4 address bits.
                for (uint32_t i = 0; i < run * 4; i++)
                    CopyDwords((cursor + 8 + i * 16) & ~15u, (source + 4 + i * 16) & ~15u, 4);
                cursor += 4 + count * 4;
            }
            source += count * 4;
            reg += count;
            mask = ShiftLeft(mask, run);
        }
        StoreBE32(device + DEVICE_CURSOR, cursor);
    }

    // CRASH_NATIVE_D3D_VERIFY: the original writes first, its bytes are kept, the native version
    // writes again from the same cursor, and the two are compared (only when the run stayed in one
    // segment, so both wrote the same memory).
    template <typename Native>
    void VerifiedFlush(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), Native native, const char* name)
    {
        static std::atomic<uint64_t> s_checked, s_mismatches;
        PPCContext saved = ctx;
        uint32_t device = ctx.r3.u32;
        uint32_t start = LoadBE32(device + DEVICE_CURSOR), end = LoadBE32(device + DEVICE_SEGMENT_END);
        original(ctx, base);
        uint32_t originalCursor = LoadBE32(device + DEVICE_CURSOR);
        if (originalCursor < start || originalCursor >= end)
            return;  // switched segment: not compared
        std::vector<uint8_t> expected(originalCursor - start);
        memcpy(expected.data(), g_memory.Translate(start + 4), expected.size());
        memset(g_memory.Translate(start + 4), 0xCD, expected.size());
        PPCContext replay = saved;
        StoreBE32(device + DEVICE_CURSOR, start);
        native(replay, base);
        uint32_t nativeCursor = LoadBE32(device + DEVICE_CURSOR);
        bool same = nativeCursor == originalCursor && !memcmp(expected.data(), g_memory.Translate(start + 4), expected.size());
        uint64_t checked = ++s_checked;
        if (!same)
        {
            uint64_t mismatches = ++s_mismatches;
            if (mismatches <= 10)
                Log("[d3d] verify %s: MISMATCH (mask %016llX reg 0x%X, cursor %08X -> original %08X, native %08X)", name, (unsigned long long)saved.r4.u64,
                    saved.r5.u32, start, originalCursor, nativeCursor);
            // Keep the original's bytes.
            memcpy(g_memory.Translate(start + 4), expected.data(), expected.size());
            StoreBE32(device + DEVICE_CURSOR, originalCursor);
        }
        if ((checked & (checked - 1)) == 0 && checked >= 1024)
            Log("[d3d] verify %s: %llu flushes compared, %llu mismatches", name, (unsigned long long)checked, (unsigned long long)s_mismatches.load());
    }
}

PPC_FUNC(sub_8226E308)
{
    if (!Enabled())
        return __imp__sub_8226E308(ctx, base);
    if (Verify())
        return VerifiedFlush(ctx, base, __imp__sub_8226E308, FlushRegisters, "registers");
    FlushRegisters(ctx, base);
}

PPC_FUNC(sub_8226E6A8)
{
    if (!Enabled())
        return __imp__sub_8226E6A8(ctx, base);
    if (Verify())
        return VerifiedFlush(ctx, base, __imp__sub_8226E6A8, FlushConstants, "constants");
    FlushConstants(ctx, base);
}

// sub_8226C3F0(device r3, entries r4, count r5): appends an INDIRECT_BUFFER packet (0xC0013F00,
// address, size) for each { size, address } entry to the primary ring (device+14744, index mask
// +14748, write index +10824) after waiting for room (sub_8226BD58), then stores the write index in
// CP_RB_WPTR. The PIX/capture paths (device+10813 bit 1, callbacks at +21380 / +21520) stay with
// the original.
PPC_FUNC(sub_8226C3F0)
{
    uint32_t device = ctx.r3.u32, entries = ctx.r4.u32, count = ctx.r5.u32;
    if (!Enabled() || (*g_memory.Translate(device + 10813) & 2) || LoadBE32(device + 21380) || LoadBE32(device + 21520))
        return __imp__sub_8226C3F0(ctx, base);
    uint32_t ring = LoadBE32(device + 14744), mask = LoadBE32(device + 14748);
    uint32_t write = LoadBE32(device + 10824);
    ctx.r3.u64 = device;
    ctx.r4.u64 = write;
    ctx.r5.u64 = count * 3;
    sub_8226BD58(ctx, base);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t size = LoadBE32(entries + i * 8) & 0xFFFFFF, address = LoadBE32(entries + i * 8 + 4);
        StoreBE32(ring + write * 4, 0xC0013F00);
        write = (write + 1) & mask;
        StoreBE32(ring + write * 4, address);
        write = (write + 1) & mask;
        StoreBE32(ring + write * 4, size);
        write = (write + 1) & mask;
    }
    StoreBE32(device + 10824, write);
    std::atomic_thread_fence(std::memory_order_release);
    gpu::WriteRegister(gpu::REG_CP_RB_WPTR, write);
    gpu::WakeCommandProcessor();
}
