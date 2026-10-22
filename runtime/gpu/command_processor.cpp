// Xenos command processor: walks the PM4 stream in the primary ring buffer (and the indirect
// buffers it points to), applies register writes, performs the memory writes, waits and
// interrupts the title synchronizes on, and writes the read pointer back. Shader loads, draws
// and swaps go to the renderer (renderer.h). Packet formats and opcode semantics follow Xenia's
// command_processor.cc.
//
// PM4 headers (big-endian dwords, like the rest of the stream):
//   type 0: [31:30]=0, [29:16]=count-1, [15]=one register, [14:0]=base register; count values
//   type 1: [31:30]=1, [21:11]=register 2, [10:0]=register 1; 2 values
//   type 2: [31:30]=2, no-op
//   type 3: [31:30]=3, [29:16]=count-1, [14:8]=opcode, [0]=predicated; count values

#include "gpu.h"
#include "renderer.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace
{
    enum Opcode : uint32_t
    {
        PM4_ME_INIT = 0x48,
        PM4_NOP = 0x10,
        PM4_INTERRUPT = 0x54,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_INDIRECT_BUFFER = 0x3F,
        PM4_WAIT_REG_MEM = 0x3C,
        PM4_REG_RMW = 0x21,
        PM4_REG_TO_MEM = 0x3E,
        PM4_MEM_WRITE = 0x3D,
        PM4_COND_WRITE = 0x45,
        PM4_EVENT_WRITE = 0x46,
        PM4_EVENT_WRITE_SHD = 0x58,
        PM4_EVENT_WRITE_EXT = 0x5A,
        PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_SET_CONSTANT = 0x2D,
        PM4_SET_CONSTANT2 = 0x55,
        PM4_LOAD_ALU_CONSTANT = 0x2F,
        PM4_SET_SHADER_CONSTANTS = 0x56,
        PM4_SET_BIN_MASK = 0x50,
        PM4_SET_BIN_SELECT = 0x51,
        PM4_SET_BIN_MASK_LO = 0x60,
        PM4_SET_BIN_MASK_HI = 0x61,
        PM4_SET_BIN_SELECT_LO = 0x62,
        PM4_SET_BIN_SELECT_HI = 0x63,
        PM4_XE_SWAP = 0x64, // written by our VdSwap, as in Xenia
        PM4_DRAW_INDX = 0x22,
        PM4_DRAW_INDX_2 = 0x36,
        PM4_IM_LOAD = 0x27,
        PM4_IM_LOAD_IMMEDIATE = 0x2B,
    };

    // Stream position: a ring (sizeDwords != 0, wraps) or a linear indirect buffer.
    struct Reader
    {
        uint32_t base;        // guest address
        uint32_t sizeDwords;  // ring size, 0 for linear
        uint32_t position;    // in dwords

        uint32_t Read()
        {
            uint32_t index = sizeDwords ? position % sizeDwords : position;
            position++;
            return LoadBE32(base + index * 4);
        }

        void Skip(uint32_t count)
        {
            position += count;
        }
    };

    std::mutex g_mutex;
    std::condition_variable g_condition;
    bool g_wake;

    std::atomic<uint32_t> g_ringBase;       // guest address
    std::atomic<uint32_t> g_ringSizeDwords;
    std::atomic<uint32_t> g_writebackAddress; // guest address, 0 = off
    uint32_t g_readPointer;

    // Predicated tiling: D3D replays a command buffer once per screen tile (bin) and marks the
    // tile-specific packets as predicated (header bit 0); they only run when the current bin
    // select and mask overlap. Only the command processor thread touches these.
    uint64_t g_binMask = ~0ull;
    uint64_t g_binSelect = ~0ull;
    // Counters written only by the command processor thread (read for the log): plain increments,
    // not atomic read-modify-writes (a lock-prefixed add per packet was half of the PM4 parsing).
    template <typename T>
    void Bump(std::atomic<T>& counter, T by = 1)
    {
        counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
    }
    std::atomic<uint64_t> g_skippedPredicated;

    std::array<std::atomic<uint64_t>, 128> g_type3Counts;
    std::atomic<uint64_t> g_type0Packets;
    std::atomic<uint64_t> g_indirectBuffers;
    std::atomic<uint32_t> g_badPackets;
    std::atomic<uint64_t> g_commandDwords;  // PM4 dwords executed (ring + indirect buffers), for the pacing log

    uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value >> 8) & 0x00FF00FF) | ((value << 8) & 0xFF00FF00); // 8 in 16
        case 2: return __builtin_bswap32(value);                                   // 8 in 32
        case 3: return (value >> 16) | (value << 16);                              // 16 in 32
        default: return value;
        }
    }

    // GPU memory accesses: the low 2 bits of the address select the endian swap; memory holds
    // the swapped value in the GPU's (little-endian) byte order.
    void WriteGpuMemory(uint32_t address, uint32_t value)
    {
        uint32_t guest = gpu::GuestFromGpuAddress(address & ~3u);
        *reinterpret_cast<volatile uint32_t*>(g_memory.Translate(guest)) = GpuSwap(value, address & 3);
    }

    uint32_t ReadGpuMemory(uint32_t address)
    {
        uint32_t guest = gpu::GuestFromGpuAddress(address & ~3u);
        return GpuSwap(*reinterpret_cast<volatile uint32_t*>(g_memory.Translate(guest)), address & 3);
    }

    bool Compare(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
        case 0: return false; // never
        case 1: return value < reference;
        case 2: return value <= reference;
        case 3: return value == reference;
        case 4: return value != reference;
        case 5: return value >= reference;
        case 6: return value > reference;
        default: return true; // always
        }
    }

    // Base register for SET_CONSTANT / LOAD_ALU_CONSTANT types.
    uint32_t ConstantBase(uint32_t type)
    {
        switch (type)
        {
        case 0: return 0x4000; // ALU constants
        case 1: return 0x4800; // fetch constants
        case 2: return 0x4900; // bool constants
        case 3: return 0x4908; // loop constants
        case 4: return 0x2000; // registers
        default: return 0;
        }
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        gpu::WriteRegister(index, value);
    }

    // `count` consecutive registers from `index` on, values read from the stream. The register
    // file above the trapped first page is stored big-endian like the PM4 stream, so when the run
    // has no side effects (not the trapped page, not the scratch registers mirrored to memory) and
    // doesn't wrap around the ring, it is a plain copy. Shader constant uploads are runs of 4-1024
    // registers, several per draw.
    void WriteRegisters(uint32_t index, Reader& reader, uint32_t count)
    {
        constexpr uint32_t TRAPPED_REGISTERS = 0x1000 / 4;
        uint32_t start = reader.sizeDwords ? reader.position % reader.sizeDwords : reader.position;
        bool linear = !reader.sizeDwords || start + count <= reader.sizeDwords;
        bool plain = index >= TRAPPED_REGISTERS && index + count <= gpu::REGISTER_COUNT &&
                     (index + count <= gpu::REG_SCRATCH_REG0 || index >= gpu::REG_SCRATCH_REG0 + 8);
        if (linear && plain)
        {
            memcpy(g_memory.Translate(GPU_MMIO_BEGIN + index * 4), g_memory.Translate(reader.base + start * 4), size_t(count) * 4);
            reader.position += count;
            return;
        }
        for (uint32_t i = 0; i < count; i++)
            WriteRegister(index + i, reader.Read());
    }

    void WritebackReadPointer(uint32_t readPointer)
    {
        gpu::WriteRegister(gpu::REG_CP_RB_RPTR, readPointer);
        if (uint32_t address = g_writebackAddress)
            StoreBE32(address, readPointer);
    }

    void ExecuteBuffer(Reader reader, uint32_t endPosition, int depth);

    // The last packets executed (address, header, first two data dwords), printed when a wait
    // gets stuck: a desynchronized stream shows up as data words parsed as headers.
    struct RecentPacket
    {
        uint32_t address, header, data0, data1;
    };
    RecentPacket g_recentPackets[64];
    uint32_t g_recentNext;

    void LogRecentPackets()
    {
        Log("[gpu] last packets executed (address: header  data0 data1):");
        for (uint32_t i = 0; i < 64; i++)
        {
            auto& packet = g_recentPackets[(g_recentNext + i) % 64];
            if (packet.header)
                Log("    0x%08X: %08X  %08X %08X", packet.address, packet.header, packet.data0, packet.data1);
        }
        Log("    SCRATCH_UMSK=0x%X SCRATCH_ADDR=0x%08X", gpu::ReadRegister(gpu::REG_SCRATCH_UMSK), gpu::ReadRegister(gpu::REG_SCRATCH_ADDR));
    }


    // WAIT_REG_MEM: blocks the command processor until the condition holds (polling).
    void WaitRegisterOrMemory(uint32_t waitInfo, uint32_t pollAddress, uint32_t reference, uint32_t mask, uint32_t interval)
    {
        auto start = std::chrono::steady_clock::now();
        bool warned = false;

        while (true)
        {
            uint32_t value;
            if (waitInfo & 0x10)
            {
                value = ReadGpuMemory(pollAddress);
            }
            else
            {
                if (pollAddress == gpu::REG_COHER_STATUS_HOST)
                    gpu::WriteRegister(pollAddress, gpu::ReadRegister(pollAddress) & ~0x80000000u); // caches are always coherent
                value = gpu::ReadRegister(pollAddress);
            }

            if (Compare(waitInfo, value & mask, reference))
                return;

            if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
            {
                warned = true;
                Log("[gpu] WAIT_REG_MEM stuck for 2 s: %s 0x%08X & 0x%08X = 0x%08X, function %u, reference 0x%08X",
                    waitInfo & 0x10 ? "memory" : "register", pollAddress, mask, value & mask, waitInfo & 7, reference);
                LogRecentPackets();
            }

            if (interval)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            else
                std::this_thread::yield();
        }
    }

    void ExecuteType3(Reader& reader, uint32_t header, int depth)
    {
        uint32_t opcode = (header >> 8) & 0x7F;
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        uint32_t dataStart = reader.position;
        Bump(g_type3Counts[opcode]);

        // Predicated packets outside the current bins are skipped; predicated swaps never run
        // (as in Xenia).
        if ((header & 1) && ((g_binSelect & g_binMask) == 0 || opcode == PM4_XE_SWAP))
        {
            Bump(g_skippedPredicated);
            reader.position = dataStart + count;
            return;
        }

        switch (opcode)
        {
        case PM4_INTERRUPT:
        {
            // Delivered synchronously, as in Xenia: D3D's callback sequence (sub_8226CA20) is
            // SCRATCH_REG0 = mask (written back to memory), WAIT mem == mask, INTERRUPT, and the
            // callback then clears its bit in that word. If the stream ran on before the callback,
            // a late clear could wipe the next sequence's write-back and hang its WAIT.
            uint32_t cpuMask = reader.Read();
            for (uint32_t cpu = 0; cpu < 6; cpu++)
            {
                if ((cpuMask >> cpu) & 1)
                    gpu::RequestInterrupt(gpu::INTERRUPT_SOURCE_COMMAND_PROCESSOR, cpu, true);
            }
            break;
        }
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t address = reader.Read();
            uint32_t length = reader.Read() & 0xFFFFF;
            Bump(g_indirectBuffers);
            if (depth < 4)
                ExecuteBuffer(Reader{ gpu::GuestFromGpuAddress(address & ~3u), 0, 0 }, length, depth + 1);
            else
                Log("[gpu] indirect buffer nested too deep, skipped");
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            uint32_t waitInfo = reader.Read();
            uint32_t pollAddress = reader.Read();
            uint32_t reference = reader.Read();
            uint32_t mask = reader.Read();
            uint32_t interval = reader.Read();
            WaitRegisterOrMemory(waitInfo, pollAddress, reference, mask, interval);
            break;
        }
        case PM4_REG_RMW:
        {
            uint32_t info = reader.Read();
            uint32_t andMask = reader.Read();
            uint32_t orMask = reader.Read();
            uint32_t index = info & 0x1FFF;
            uint32_t value = gpu::ReadRegister(index);
            value &= (info >> 31) & 1 ? gpu::ReadRegister(andMask & 0x1FFF) : andMask;
            value |= (info >> 30) & 1 ? gpu::ReadRegister(orMask & 0x1FFF) : orMask;
            WriteRegister(index, value);
            break;
        }
        case PM4_REG_TO_MEM:
        {
            uint32_t index = reader.Read() & 0x1FFF;
            uint32_t address = reader.Read();
            WriteGpuMemory(address, gpu::ReadRegister(index));
            break;
        }
        case PM4_MEM_WRITE:
        {
            uint32_t address = reader.Read();
            for (uint32_t i = 1; i < count; i++, address += 4)
                WriteGpuMemory(address, reader.Read());
            break;
        }
        case PM4_COND_WRITE:
        {
            uint32_t waitInfo = reader.Read();
            uint32_t pollAddress = reader.Read();
            uint32_t reference = reader.Read();
            uint32_t mask = reader.Read();
            uint32_t writeAddress = reader.Read();
            uint32_t writeData = reader.Read();
            uint32_t value = waitInfo & 0x10 ? ReadGpuMemory(pollAddress) : gpu::ReadRegister(pollAddress);
            if (Compare(waitInfo, value & mask, reference))
            {
                if (waitInfo & 0x100)
                    WriteGpuMemory(writeAddress, writeData);
                else
                    WriteRegister(writeAddress, writeData);
            }
            break;
        }
        case PM4_EVENT_WRITE:
            WriteRegister(gpu::REG_VGT_EVENT_INITIATOR, reader.Read() & 0x3F);
            break;
        case PM4_EVENT_WRITE_SHD:
        {
            uint32_t initiator = reader.Read();
            uint32_t address = reader.Read();
            uint32_t value = reader.Read();
            WriteRegister(gpu::REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
            // Bit 31: write the swap counter instead of the value (as in Xenia).
            WriteGpuMemory(address, (initiator >> 31) & 1 ? uint32_t(gpu::VblankCount()) : value);
            break;
        }
        case PM4_EVENT_WRITE_EXT:
        {
            uint32_t initiator = reader.Read();
            uint32_t address = reader.Read();
            WriteRegister(gpu::REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
            // Screen extents (min/max x and y in units of 8 pixels, min/max z) of the previous
            // draws. D3D turns them into the bin masks of the predicated draws for the other
            // tiles; faked to "everything" like Xenia does.
            const uint16_t extents[] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
            auto* destination = reinterpret_cast<uint16_t*>(g_memory.Translate(gpu::GuestFromGpuAddress(address & ~3u)));
            bool swap = (address & 3) == 1 || (address & 3) == 2;
            for (size_t i = 0; i < std::size(extents); i++)
                destination[i] = swap ? __builtin_bswap16(extents[i]) : extents[i];
            break;
        }
        case PM4_EVENT_WRITE_ZPD:
            WriteRegister(gpu::REG_VGT_EVENT_INITIATOR, reader.Read() & 0x3F);
            break;
        case PM4_SET_CONSTANT:
        {
            uint32_t offsetType = reader.Read();
            uint32_t index = (offsetType & 0x7FF) + ConstantBase((offsetType >> 16) & 0xFF);
            WriteRegisters(index, reader, count - 1);
            break;
        }
        case PM4_SET_CONSTANT2:
        case PM4_SET_SHADER_CONSTANTS:
        {
            uint32_t index = reader.Read() & 0xFFFF;
            WriteRegisters(index, reader, count - 1);
            break;
        }
        case PM4_LOAD_ALU_CONSTANT:
        {
            uint32_t address = reader.Read() & 0x3FFFFFFF;
            uint32_t offsetType = reader.Read();
            uint32_t size = reader.Read() & 0xFFF;
            uint32_t index = (offsetType & 0x7FF) + ConstantBase((offsetType >> 16) & 0xFF);
            uint32_t source = gpu::GuestFromGpuAddress(address);
            Reader constants{ source, 0, 0 };
            WriteRegisters(index, constants, size);
            break;
        }
        case PM4_SET_BIN_MASK:
            g_binMask = (uint64_t(reader.Read()) << 32) | reader.Read();
            break;
        case PM4_SET_BIN_SELECT:
            g_binSelect = (uint64_t(reader.Read()) << 32) | reader.Read();
            break;
        case PM4_SET_BIN_MASK_LO:
            g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | reader.Read();
            break;
        case PM4_SET_BIN_MASK_HI:
            g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(reader.Read()) << 32);
            break;
        case PM4_SET_BIN_SELECT_LO:
            g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | reader.Read();
            break;
        case PM4_SET_BIN_SELECT_HI:
            g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(reader.Read()) << 32);
            break;
        case PM4_XE_SWAP:
        {
            gpu::CountExecutedSwap();
            // 'SWAP', front buffer, width, height, then the front buffer texture fetch constant.
            uint32_t data[10] = {};
            for (uint32_t i = 0; i < std::min(count, 10u); i++)
                data[i] = reader.Read();
            gpu::renderer::Swap(data[1], data[2], data[3], count >= 10 ? data + 4 : nullptr);
            break;
        }
        case PM4_IM_LOAD_IMMEDIATE:
        {
            // Shader microcode embedded in the packet: type (0 vertex, 1 pixel), start << 16 | size.
            uint32_t type = reader.Read();
            uint32_t sizeDwords = reader.Read() & 0xFFFF;
            sizeDwords = std::min(sizeDwords, count - 2);
            static std::vector<uint32_t> ucode;
            ucode.resize(sizeDwords);
            for (uint32_t i = 0; i < sizeDwords; i++)
                ucode[i] = reader.Read();
            gpu::renderer::LoadShader(type & 1 ? gpu::ShaderStage::Pixel : gpu::ShaderStage::Vertex, ucode.data(), sizeDwords);
            break;
        }
        case PM4_IM_LOAD:
        {
            // Shader microcode in memory: address | type, start << 16 | size. The game loads a
            // vertex and a pixel shader for nearly every draw, from the same few addresses: while
            // the bytes there are the ones seen last time (memcmp), the shader found then is bound
            // again, without swapping and hashing the microcode.
            uint32_t addressType = reader.Read();
            uint32_t sizeDwords = reader.Read() & 0xFFFF;
            uint32_t address = gpu::GuestFromGpuAddress(addressType & ~3u);
            gpu::ShaderStage stage = addressType & 1 ? gpu::ShaderStage::Pixel : gpu::ShaderStage::Vertex;
            struct Cached
            {
                std::vector<uint32_t> raw;  // guest bytes (big-endian)
                void* shader = nullptr;
            };
            static std::unordered_map<uint64_t, Cached> s_cache;
            const uint32_t* guest = reinterpret_cast<const uint32_t*>(g_memory.Translate(address));
            Cached& cached = s_cache[(uint64_t(address) << 1 | (addressType & 1)) | (uint64_t(sizeDwords) << 33)];
            if (cached.shader && cached.raw.size() == sizeDwords && memcmp(cached.raw.data(), guest, size_t(sizeDwords) * 4) == 0)
            {
                gpu::renderer::BindShader(stage, cached.shader);
                break;
            }
            static std::vector<uint32_t> ucode;
            ucode.resize(sizeDwords);
            for (uint32_t i = 0; i < sizeDwords; i++)
                ucode[i] = LoadBE32(address + i * 4);
            cached.raw.assign(guest, guest + sizeDwords);
            cached.shader = gpu::renderer::LoadShaderHandle(stage, ucode.data(), sizeDwords);
            break;
        }
        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
        {
            // DRAW_INDX: viz query, VGT_DRAW_INITIATOR, [VGT_DMA_BASE, VGT_DMA_SIZE] for DMA indices.
            // DRAW_INDX_2: VGT_DRAW_INITIATOR, then the indices for an immediate source.
            uint32_t remaining = count;
            if (opcode == PM4_DRAW_INDX)
            {
                reader.Read();
                remaining--;
            }
            gpu::DrawPacket draw{};
            draw.drawInitiator = reader.Read();
            remaining--;
            WriteRegister(0x21FC, draw.drawInitiator); // VGT_DRAW_INITIATOR
            uint32_t source = (draw.drawInitiator >> 6) & 3;
            if (source == 0 && remaining >= 2)
            {
                draw.indexBase = reader.Read();
                draw.indexSize = reader.Read();
                WriteRegister(0x21FA, draw.indexBase); // VGT_DMA_BASE
                WriteRegister(0x21FB, draw.indexSize); // VGT_DMA_SIZE
            }
            else if (source == 1)
            {
                uint32_t index = reader.sizeDwords ? reader.position % reader.sizeDwords : reader.position;
                draw.immediateIndices = reinterpret_cast<const uint32_t*>(g_memory.Translate(reader.base + index * 4));
                draw.immediateDwords = remaining;
            }
            gpu::renderer::Draw(draw);
            break;
        }
        default:
            // Draws, shader loads, ME_INIT, NOP, state invalidation...: counted, not executed.
            break;
        }

        // Whatever the handler read, continue right after the packet.
        reader.position = dataStart + count;
    }

    // Executes packets until `reader.position` reaches `endPosition` (dwords).
    void ExecuteBuffer(Reader reader, uint32_t endPosition, int depth)
    {
        if (!reader.sizeDwords)
            Bump(g_commandDwords, uint64_t(endPosition - reader.position));
        while (reader.position != endPosition)
        {
            uint32_t header = reader.Read();
            {
                uint32_t next = reader.sizeDwords ? reader.position % reader.sizeDwords : reader.position;
                uint32_t after = reader.sizeDwords ? (reader.position + 1) % reader.sizeDwords : reader.position + 1;
                uint32_t self = reader.sizeDwords ? (reader.position + reader.sizeDwords - 1) % reader.sizeDwords : reader.position - 1;
                g_recentPackets[g_recentNext++ % 64] = { reader.base + self * 4, header, LoadBE32(reader.base + next * 4), LoadBE32(reader.base + after * 4) };
            }
            switch (header >> 30)
            {
            case 0:
            {
                uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                uint32_t index = header & 0x7FFF;
                bool oneRegister = (header >> 15) & 1;
                if (oneRegister)
                {
                    for (uint32_t i = 0; i < count; i++)
                        WriteRegister(index, reader.Read());
                }
                else
                {
                    WriteRegisters(index, reader, count);
                }
                Bump(g_type0Packets);
                break;
            }
            case 1:
            {
                uint32_t value1 = reader.Read();
                uint32_t value2 = reader.Read();
                WriteRegister(header & 0x7FF, value1);
                WriteRegister((header >> 11) & 0x7FF, value2);
                break;
            }
            case 2:
                break;
            case 3:
                ExecuteType3(reader, header, depth);
                break;
            }

            // A malformed packet can step over the end; ring positions are compared modulo size.
            if (reader.sizeDwords == 0 && reader.position > endPosition)
            {
                if (g_badPackets.fetch_add(1) < 8)
                    Log("[gpu] packet overran its buffer (header 0x%08X)", header);
                return;
            }
            if (reader.sizeDwords)
                reader.position %= reader.sizeDwords;
        }
    }

    void CommandProcessorThread()
    {
        Log("[gpu] command processor thread started");
        // The thread the frame rate depends on most (it translates every draw): ahead of the
        // normal-priority ones, which matters most with the window out of focus.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        SetThreadDescription(GetCurrentThread(), L"gpu command processor");
        while (true)
        {
            {
                std::unique_lock lock(g_mutex);
                // Also polls: a CP_RB_WPTR write can slip past the trap (see gpu.cpp).
                g_condition.wait_for(lock, std::chrono::milliseconds(2), [] { return g_wake; });
                g_wake = false;
            }

            uint32_t ringBase = g_ringBase;
            uint32_t sizeDwords = g_ringSizeDwords;
            if (!ringBase || !sizeDwords)
                continue;

            uint32_t writePointer = gpu::ReadRegister(gpu::REG_CP_RB_WPTR) % sizeDwords;
            if (writePointer == g_readPointer)
                continue;

            static uint32_t loggedBatches;
            if (loggedBatches++ < 5)
                Log("[gpu] executing ring 0x%X..0x%X", g_readPointer, writePointer);

            ExecuteBuffer(Reader{ ringBase, sizeDwords, g_readPointer }, writePointer, 0);
            g_readPointer = writePointer;
            WritebackReadPointer(writePointer);
        }
    }
}

namespace gpu
{
    void StartCommandProcessor()
    {
        std::thread(CommandProcessorThread).detach();
    }

    void SetRingBuffer(uint32_t gpuAddress, uint32_t sizeLog2)
    {
        uint32_t sizeBytes = 1u << (sizeLog2 + 3);
        g_readPointer = 0;
        g_ringSizeDwords = sizeBytes / 4;
        g_ringBase = GuestFromGpuAddress(gpuAddress);
        Log("[gpu] ring buffer at 0x%08X (guest 0x%08X), 0x%X bytes", gpuAddress, GuestFromGpuAddress(gpuAddress), sizeBytes);
    }

    void SetReadPointerWriteback(uint32_t gpuAddress, uint32_t blockSizeLog2)
    {
        g_writebackAddress = GuestFromGpuAddress(gpuAddress);
        Log("[gpu] read pointer write-back at 0x%08X (guest 0x%08X), block 2^%u", gpuAddress, GuestFromGpuAddress(gpuAddress), blockSizeLog2);
    }

    void WakeCommandProcessor()
    {
        {
            std::lock_guard lock(g_mutex);
            g_wake = true;
        }
        g_condition.notify_one();
    }

    uint64_t TakeCommandDwords()
    {
        return g_commandDwords.exchange(0, std::memory_order_relaxed);
    }

    void LogCommandProcessorStats()
    {
        Log("[gpu] command processor: read pointer 0x%X, %llu type-0 packet(s), %llu indirect buffer(s), %llu predicated packet(s) skipped; type-3 opcodes:",
            g_readPointer, (unsigned long long)g_type0Packets.load(), (unsigned long long)g_indirectBuffers.load(),
            (unsigned long long)g_skippedPredicated.load());
        for (uint32_t opcode = 0; opcode < g_type3Counts.size(); opcode++)
        {
            if (uint64_t count = g_type3Counts[opcode])
                Log("    0x%02X x%llu", opcode, (unsigned long long)count);
        }
    }
}
