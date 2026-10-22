// xboxkrnl Vd* (video driver) imports and xam's XGetVideoMode: a fixed 1280x720 60 Hz widescreen
// HDMI display, the ring buffer / write-back / interrupt setup the command processor needs, and
// VdSwap. Structure layouts and values follow Xenia's xboxkrnl_video.cc.

#include "gpu.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <cstring>

namespace
{
    constexpr uint32_t DISPLAY_WIDTH = 1280;
    constexpr uint32_t DISPLAY_HEIGHT = 720;
    constexpr float REFRESH_RATE = 60.0f;

    uint32_t FloatBits(float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    void StoreBE16(uint8_t* base, uint32_t guest, uint16_t value)
    {
        PPC_STORE_U16(guest, value);
    }

    // X_VIDEO_MODE (0x30 bytes).
    void WriteVideoMode(uint8_t* base, uint32_t mode)
    {
        memset(base + mode, 0, 0x30);
        PPC_STORE_U32(mode + 0x00, DISPLAY_WIDTH);
        PPC_STORE_U32(mode + 0x04, DISPLAY_HEIGHT);
        PPC_STORE_U32(mode + 0x08, 0);                       // interlaced
        PPC_STORE_U32(mode + 0x0C, 1);                       // widescreen
        PPC_STORE_U32(mode + 0x10, 1);                       // high definition
        PPC_STORE_U32(mode + 0x14, FloatBits(REFRESH_RATE)); // refresh rate
        PPC_STORE_U32(mode + 0x18, 1);                       // video standard: NTSC
        PPC_STORE_U32(mode + 0x1C, 0x4A);                    // unknown, as on retail consoles
        PPC_STORE_U32(mode + 0x20, 0x01);
    }
}

// u32 VdInitializeEngines(u32 Unknown, void* Callback, void* Argument, u32* PfpUcode, u32* MeUcode)
PPC_FUNC(__imp__VdInitializeEngines)
{
    KERNEL_IMPORT_ENTER(VdInitializeEngines);
    KERNEL_LOG("[gpu] VdInitializeEngines(0x%08X, callback=0x%08X, 0x%08X) lr=0x%08X -> 1", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, GuestLR(ctx));
    ctx.r3.u64 = 1;
}

// void VdShutdownEngines()
PPC_FUNC(__imp__VdShutdownEngines)
{
    KERNEL_IMPORT_ENTER(VdShutdownEngines);
    KERNEL_LOG("[gpu] VdShutdownEngines() lr=0x%08X", GuestLR(ctx));
}

// void VdSetGraphicsInterruptCallback(void* Callback, void* UserData)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    KERNEL_IMPORT_ENTER(VdSetGraphicsInterruptCallback);
    gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
    KERNEL_LOG("[gpu] VdSetGraphicsInterruptCallback(0x%08X, 0x%08X) lr=0x%08X", ctx.r3.u32, ctx.r4.u32, GuestLR(ctx));
}

// void VdInitializeRingBuffer(u32 GpuAddress, u32 SizeLog2): size is 2^(SizeLog2 + 3) bytes.
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    KERNEL_IMPORT_ENTER(VdInitializeRingBuffer);
    gpu::SetRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// void VdEnableRingBufferRPtrWriteBack(u32 GpuAddress, u32 BlockSizeLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    KERNEL_IMPORT_ENTER(VdEnableRingBufferRPtrWriteBack);
    gpu::SetReadPointerWriteback(ctx.r3.u32, ctx.r4.u32);
}

// void VdSetSystemCommandBufferGpuIdentifierAddress(u32 Address)
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    KERNEL_IMPORT_ENTER(VdSetSystemCommandBufferGpuIdentifierAddress);
    KERNEL_LOG("[gpu] VdSetSystemCommandBufferGpuIdentifierAddress(0x%08X) lr=0x%08X", ctx.r3.u32, GuestLR(ctx));
}

// void VdGetSystemCommandBuffer(void* Buffer, u32* Unknown): Buffer is a 0x94-byte structure the
// caller (D3D's swap, sub_82274080) reads back after the call (+4 and +8 go into the device), so all
// of it must be written: zeroed, with Xenia's placeholder in the first dword.
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    KERNEL_IMPORT_ENTER(VdGetSystemCommandBuffer);
    if (ctx.r3.u32)
    {
        memset(base + ctx.r3.u32, 0, 0x94);
        PPC_STORE_U32(ctx.r3.u32, 0xBEEF0000);
    }
    if (ctx.r4.u32)
        PPC_STORE_U32(ctx.r4.u32, 0xBEEF0001);
    KERNEL_LOG("[gpu] VdGetSystemCommandBuffer(0x%08X, 0x%08X) lr=0x%08X", ctx.r3.u32, ctx.r4.u32, GuestLR(ctx));
}

// void VdQueryVideoMode(X_VIDEO_MODE* Mode)
PPC_FUNC(__imp__VdQueryVideoMode)
{
    KERNEL_IMPORT_ENTER(VdQueryVideoMode);
    WriteVideoMode(base, ctx.r3.u32);
    KERNEL_LOG("[gpu] VdQueryVideoMode(0x%08X) lr=0x%08X -> %ux%u", ctx.r3.u32, GuestLR(ctx), DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

// void XGetVideoMode(X_VIDEO_MODE* Mode) (xam)
PPC_FUNC(__imp__XGetVideoMode)
{
    KERNEL_IMPORT_ENTER(XGetVideoMode);
    WriteVideoMode(base, ctx.r3.u32);
    KERNEL_LOG("[gpu] XGetVideoMode(0x%08X) lr=0x%08X -> %ux%u", ctx.r3.u32, GuestLR(ctx), DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

// u32 VdQueryVideoFlags(): bit 0 = widescreen, bit 1 = 1024-wide, bit 2 = 1920-wide.
PPC_FUNC(__imp__VdQueryVideoFlags)
{
    KERNEL_IMPORT_ENTER(VdQueryVideoFlags);
    KERNEL_LOG("[gpu] VdQueryVideoFlags() lr=0x%08X -> 1", GuestLR(ctx));
    ctx.r3.u64 = 1;
}

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* Info) (0x58 bytes)
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    KERNEL_IMPORT_ENTER(VdGetCurrentDisplayInformation);

    uint32_t info = ctx.r3.u32;
    memset(base + info, 0, 0x58);
    StoreBE16(base, info + 0x00, DISPLAY_WIDTH);        // front buffer width
    StoreBE16(base, info + 0x02, DISPLAY_HEIGHT);       // front buffer height
    // Scaler parameters at 0x08: source rect, scaled output size, filter types.
    PPC_STORE_U32(info + 0x08 + 0x08, DISPLAY_WIDTH);   // source rect x2
    PPC_STORE_U32(info + 0x08 + 0x0C, DISPLAY_HEIGHT);  // source rect y2
    PPC_STORE_U32(info + 0x08 + 0x10, DISPLAY_WIDTH);   // scaled output width
    PPC_STORE_U32(info + 0x08 + 0x14, DISPLAY_HEIGHT);  // scaled output height
    PPC_STORE_U32(info + 0x08 + 0x18, 1);               // vertical filter type
    PPC_STORE_U32(info + 0x08 + 0x28, 1);               // horizontal filter type
    StoreBE16(base, info + 0x48, DISPLAY_WIDTH);        // display width
    StoreBE16(base, info + 0x4A, DISPLAY_HEIGHT);       // display height
    PPC_STORE_U32(info + 0x4C, FloatBits(REFRESH_RATE));
    PPC_STORE_U32(info + 0x50, 0);                      // interlaced
    StoreBE16(base, info + 0x56, DISPLAY_WIDTH);        // actual display width

    KERNEL_LOG("[gpu] VdGetCurrentDisplayInformation(0x%08X) lr=0x%08X", info, GuestLR(ctx));
}

// void VdGetCurrentDisplayGamma(u32* Type, float* Power)
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    KERNEL_IMPORT_ENTER(VdGetCurrentDisplayGamma);
    if (ctx.r3.u32)
        PPC_STORE_U32(ctx.r3.u32, 2);
    if (ctx.r4.u32)
        PPC_STORE_U32(ctx.r4.u32, FloatBits(2.22222233f));
    KERNEL_LOG("[gpu] VdGetCurrentDisplayGamma lr=0x%08X -> type 2, power 2.22", GuestLR(ctx));
}

// u32 VdSetDisplayMode(u32 Mode)
PPC_FUNC(__imp__VdSetDisplayMode)
{
    KERNEL_IMPORT_ENTER(VdSetDisplayMode);
    KERNEL_LOG("[gpu] VdSetDisplayMode(0x%08X) lr=0x%08X -> 0", ctx.r3.u32, GuestLR(ctx));
    ctx.r3.u64 = 0;
}

// u32 VdIsHSIOTrainingSucceeded()
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    KERNEL_IMPORT_ENTER(VdIsHSIOTrainingSucceeded);
    ctx.r3.u64 = 1;
}

// u32 VdRetrainEDRAM(u32 Unknown)
PPC_FUNC(__imp__VdRetrainEDRAM)
{
    KERNEL_IMPORT_ENTER(VdRetrainEDRAM);
    ctx.r3.u64 = 0;
}

// u32 VdRetrainEDRAMWorker(u32 Unknown)
PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    KERNEL_IMPORT_ENTER(VdRetrainEDRAMWorker);
    ctx.r3.u64 = 0;
}

// u32 VdEnableDisableClockGating(u32 Enable)
PPC_FUNC(__imp__VdEnableDisableClockGating)
{
    KERNEL_IMPORT_ENTER(VdEnableDisableClockGating);
    ctx.r3.u64 = 0;
}

// u32 VdCallGraphicsNotificationRoutines(u32 Unknown, void* Data, u32 Size)
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    KERNEL_IMPORT_ENTER(VdCallGraphicsNotificationRoutines);
    KERNEL_LOG("[gpu] VdCallGraphicsNotificationRoutines(0x%X, 0x%08X, 0x%X) lr=0x%08X -> 0", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, GuestLR(ctx));
    ctx.r3.u64 = 0;
}

// u32 VdPersistDisplay(u32 Unknown, u32* Buffer): *Buffer gets physical memory the title later
// frees with MmFreePhysicalMemory.
PPC_FUNC(__imp__VdPersistDisplay)
{
    KERNEL_IMPORT_ENTER(VdPersistDisplay);
    uint32_t buffer = 0;
    if (ctx.r4.u32)
    {
        buffer = AllocatePhysicalMemory(64, 32);
        PPC_STORE_U32(ctx.r4.u32, buffer);
    }
    KERNEL_LOG("[gpu] VdPersistDisplay(0x%X, 0x%08X) lr=0x%08X -> 1 (buffer 0x%08X)", ctx.r3.u32, ctx.r4.u32, GuestLR(ctx), buffer);
    ctx.r3.u64 = 1;
}

// u32 VdInitializeScalerCommandBuffer(..., u32* Destination (11th), u32 DestinationDwords (12th)):
// number of dwords written to Destination, which the title copies into its command buffer.
// No scaler commands are needed without real scan-out, so nothing is written.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    KERNEL_IMPORT_ENTER(VdInitializeScalerCommandBuffer);
    KERNEL_LOG("[gpu] VdInitializeScalerCommandBuffer(dest=0x%08X, %u dwords) lr=0x%08X -> 0",
               GuestStackArgument(ctx, base, 10), GuestStackArgument(ctx, base, 11), GuestLR(ctx));
    ctx.r3.u64 = 0;
}

// void VdSwap(u32* Buffer, void* FetchConstant, void* Unknown2, void* Unknown3, void* Unknown4,
//             u32* FrontBuffer, u32* TextureFormat, u32* ColorSpace, u32* Width, u32* Height)
// The title reserves 64 dwords in its command buffer and passes the first one (D3D's write cursor
// points at the last dword written, so Buffer = cursor + 4). As in Xenia, the 64 dwords become a
// swap token for the command processor: [XE_SWAP 'SWAP' front width height fetch[6]] and NOPs.
// Nothing outside them may be touched: Buffer - 4 is the end of the previous packet.
PPC_FUNC(__imp__VdSwap)
{
    KERNEL_IMPORT_ENTER(VdSwap);

    uint32_t buffer = ctx.r3.u32;
    uint32_t frontBuffer = ctx.r8.u32 ? PPC_LOAD_U32(ctx.r8.u32) : 0;
    uint32_t widthPtr = GuestStackArgument(ctx, base, 8);
    uint32_t heightPtr = GuestStackArgument(ctx, base, 9);
    uint32_t width = widthPtr ? PPC_LOAD_U32(widthPtr) : 0;
    uint32_t height = heightPtr ? PPC_LOAD_U32(heightPtr) : 0;

    constexpr uint32_t NOP = 0x80000000;                                    // type 2
    constexpr uint32_t XE_SWAP_HEADER = 0xC0000000 | (9 << 16) | (0x64 << 8); // type 3, 10 dwords

    // [XE_SWAP 'SWAP' front width height fetch0..fetch5]: the front buffer's texture fetch
    // constant (r4, 6 dwords) tells the presenter its format.
    PPC_STORE_U32(buffer + 0x00, XE_SWAP_HEADER);
    PPC_STORE_U32(buffer + 0x04, 0x53574150); // 'SWAP'
    PPC_STORE_U32(buffer + 0x08, frontBuffer & 0x1FFFFFFF);
    PPC_STORE_U32(buffer + 0x0C, width);
    PPC_STORE_U32(buffer + 0x10, height);
    uint32_t fetchConstant = ctx.r4.u32;
    for (uint32_t i = 0; i < 6; i++)
        PPC_STORE_U32(buffer + 0x14 + i * 4, fetchConstant ? PPC_LOAD_U32(fetchConstant + i * 4) : 0);
    for (uint32_t i = 11; i < 64; i++)
        PPC_STORE_U32(buffer + i * 4, NOP);

    gpu::CountVdSwap(frontBuffer, width, height);
    gpu::PaceFrame();

}
