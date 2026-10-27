#pragma once

#include <cstdint>

// Xenos GPU: register window, command processor and interrupts. Rendering is in renderer.h.
//
// - Registers: the MMIO window at GPU_MMIO_BEGIN (kernel/memory.h), register N at +N*4, big-endian
//   as the guest writes it. The first page (registers 0x000-0x3FF, which include CP_RB_WPTR) is
//   read-only: a guest write faults, the vectored handler lets the store complete by single-stepping
//   it and then reports it (gpu.cpp). Registers in that page are kept in a host array; the rest
//   live in the window memory, shared by the guest and the command processor.
// - Command processor (command_processor.cpp): a host thread that walks the PM4 stream in the ring
//   buffer from VdInitializeRingBuffer and writes the read pointer back.
// - Interrupts: a runtime guest thread calls the VdSetGraphicsInterruptCallback callback with
//   source 0 at ~60 Hz (vblank) and source 1 for PM4_INTERRUPT packets.

namespace gpu
{
    constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
    constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
    constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A2F;
    constexpr uint32_t REG_D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x1844;
    constexpr uint32_t REG_D1MODE_V_COUNTER = 0x194C;
    constexpr uint32_t REG_INTERRUPT_STATUS = 0x1951;
    constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;

    constexpr uint32_t REGISTER_COUNT = (0x7FD00000 - 0x7FC80000) / 4;

    constexpr uint32_t INTERRUPT_SOURCE_VBLANK = 0;
    constexpr uint32_t INTERRUPT_SOURCE_COMMAND_PROCESSOR = 1;

    // Commits the register window, installs the write trap and starts the command processor and
    // interrupt threads (idle until the title sets them up).
    void Init();

    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);

    // Guest address for a GPU (physical) address: physical memory is mapped at 0xA0000000.
    inline uint32_t GuestFromGpuAddress(uint32_t address) { return 0xA0000000 | (address & 0x1FFFFFFF); }

    // Command processor (command_processor.cpp).
    void StartCommandProcessor();
    void SetRingBuffer(uint32_t gpuAddress, uint32_t sizeLog2);
    void SetReadPointerWriteback(uint32_t gpuAddress, uint32_t blockSizeLog2);
    void WakeCommandProcessor();
    // PM4 dwords executed from indirect buffers since the last call (the renderer's pacing log).
    uint64_t TakeCommandDwords();
    void LogCommandProcessorStats();

    // Interrupts (gpu.cpp).
    void SetInterruptCallback(uint32_t callback, uint32_t userData);
    // Queues an interrupt for the interrupt thread; with `wait`, returns once the callback ran.
    void RequestInterrupt(uint32_t source, uint32_t cpu, bool wait = false);
    uint64_t VblankCount();
    // Frame cap through the guest vblank rate (2 vblanks per game frame): 30 = original, 0 = no cap
    // (1000 Hz). Takes effect at the next vblank.
    void SetFrameLimit(uint32_t fps);
    // VdSwap (game thread): with the 60/120 fps limits, waits until the frame's slot (SetFrameLimit).
    void PaceFrame();
    // VdSwap (game thread): while the app is in the background (window.h, Suspended), holds the game
    // here, between two frames, once its own pause menu is up (input::pc::RequestBackgroundPause);
    // the vblank interrupts stop with it. Returns at once otherwise.
    void HoldWhileSuspended();

    // Frames: VdSwap calls and PM4 swap packets the command processor executed.
    void CountVdSwap(uint32_t frontBuffer, uint32_t width, uint32_t height);
    void CountExecutedSwap();
}
