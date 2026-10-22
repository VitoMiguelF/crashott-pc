// Unsigned 32-bit seeks for the game's file reader, so .rcf archives (virtual ones with mods) can
// pass 2 GB.
//
// core::XenonDrive::vf14 (sub_82296ED0) is the read of the game's core file layer:
//   r3 = this, r4 = file handle, r7 = offset (u32), r8 = buffer, r9 = size, r10 = u32* bytes read
// It calls the XAPI SetFilePointer (sub_823E8220) as SetFilePointer(file, offset, NULL, FILE_BEGIN):
// with no high dword the distance is a signed LONG, so an offset >= 2 GB becomes negative and fails
// (ERROR_NEGATIVE_SEEK); the read then fails and the game shows its dirty disc error (seen with a
// 2.65 GB virtual default.rcf: XamShowDirtyDiscErrorUI, then a crash in sub_823EA7E8). RCF directory
// offsets are u32, so the fix is to pass a zero high dword: the same call, unsigned. Offsets below
// 2 GB keep the original code path.

#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../diagnostics.h"

#include <atomic>

extern "C" PPC_FUNC(__imp__sub_82296ED0);
PPC_EXTERN_FUNC(sub_823E8220);  // SetFilePointer
PPC_EXTERN_FUNC(sub_823E87C8);  // ReadFile
PPC_EXTERN_FUNC(sub_82265440);  // GetLastError
PPC_EXTERN_FUNC(sub_82296A90);  // XenonDrive: error code -> drive status

PPC_FUNC(sub_82296ED0)
{
    uint32_t offset = ctx.r7.u32;
    if (offset < 0x80000000u)
    {
        __imp__sub_82296ED0(ctx, base);
        return;
    }

    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true))
        Log("[fs] XenonDrive::Read at 0x%08X (past 2 GB): seeking with an unsigned offset", offset);

    uint32_t self = ctx.r3.u32, file = ctx.r4.u32, buffer = ctx.r8.u32, size = ctx.r9.u32, bytesRead = ctx.r10.u32;
    uint32_t stack = ctx.r1.u32;
    uint32_t frame = stack - 0x70;  // back chain + callee save area + the high dword
    PPC_STORE_U32(frame, stack);
    ctx.r1.u32 = frame;
    uint32_t high = frame + 0x60;
    PPC_STORE_U32(high, 0);

    ctx.r3.u64 = file;
    ctx.r4.u64 = offset;
    ctx.r5.u64 = high;
    ctx.r6.u64 = 0;  // FILE_BEGIN
    sub_823E8220(ctx, base);
    bool ok = ctx.r3.u32 == offset && PPC_LOAD_U32(high) == 0;
    if (ok)
    {
        ctx.r3.u64 = file;
        ctx.r4.u64 = buffer;
        ctx.r5.u64 = size;
        ctx.r6.u64 = bytesRead;
        ctx.r7.u64 = 0;  // no OVERLAPPED
        sub_823E87C8(ctx, base);
        ok = ctx.r3.u32 != 0;
    }
    if (ok)
    {
        PPC_STORE_U32(bytesRead, size);
        PPC_STORE_U32(self + 208, 0);
        ctx.r3.u64 = 1;
    }
    else
    {
        sub_82265440(ctx, base);
        ctx.r4.u64 = ctx.r3.u64;
        ctx.r3.u64 = self;
        sub_82296A90(ctx, base);
        PPC_STORE_U32(self + 208, ctx.r3.u32);
        ctx.r3.u64 = 0;
        Log("[fs] XenonDrive::Read at 0x%08X (0x%X bytes) failed", offset, size);
    }
    ctx.r1.u32 = stack;
}
