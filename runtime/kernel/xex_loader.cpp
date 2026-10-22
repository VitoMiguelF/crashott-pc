#include "xex_loader.h"
#include "../cpu/ppc_registers.h"
#include "memory.h"
#include "../diagnostics.h"
#include "../symbols.h"
#include "../xex_image/xex_image.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t XEX_HEADER_IMPORT_LIBRARIES = 0x000103FF;
    constexpr uint32_t XEX_HEADER_ENTRY_POINT = 0x00010100;
    constexpr uint32_t XEX_HEADER_DEFAULT_STACK_SIZE = 0x00020200;

    std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return {};

        std::vector<uint8_t> data(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), data.size());
        return data;
    }

    uint32_t ReadBE32(const std::vector<uint8_t>& data, size_t offset)
    {
        uint32_t value;
        memcpy(&value, data.data() + offset, sizeof(value));
        return __builtin_bswap32(value);
    }

    constexpr uint32_t XEX_HEADER_EXECUTION_INFO = 0x00040006;
    uint32_t g_titleId;

    uint16_t ReadBE16(const std::vector<uint8_t>& data, size_t offset)
    {
        uint16_t value;
        memcpy(&value, data.data() + offset, sizeof(value));
        return __builtin_bswap16(value);
    }

    // Returns the offset of an optional header's data, or the value itself for inline keys
    // (low byte 0x00), like XenonUtils' getOptHeaderPtr. 0 when the key is absent.
    uint32_t FindOptionalHeader(const std::vector<uint8_t>& xex, uint32_t key)
    {
        uint32_t count = ReadBE32(xex, 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            if (ReadBE32(xex, 0x18 + i * 8) == key)
                return ReadBE32(xex, 0x18 + i * 8 + 4);
        }
        return 0;
    }

    void InitXboxKrnlVersion(uint32_t guest)
    {
        // 2.0.17559.0, the last dashboard kernel (same as Xenia).
        StoreBE16(guest + 0, 2);
        StoreBE16(guest + 2, 0);
        StoreBE16(guest + 4, 17559);
        StoreBE16(guest + 6, 0);
    }

    void UpdateKeTimeStampBundle(uint32_t guest)
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        uint64_t systemTime = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        uint64_t ticks = GetTickCount64();
        StoreBE64(guest + 0x00, ticks * 10000);
        StoreBE64(guest + 0x08, systemTime);
        StoreBE32(guest + 0x10, uint32_t(ticks));
    }

    void InitKeTimeStampBundle(uint32_t guest)
    {
        // X_TIME_STAMP_BUNDLE: interrupt time, system time (100 ns since 1601), tick count (ms).
        // XAPI's GetTickCount reads the tick count from here, so it has to advance: a host thread
        // refreshes it every millisecond, as Xenia's kernel timer does. With a frozen snapshot the
        // title's timers never expire and boot stalls on a black screen.
        UpdateKeTimeStampBundle(guest);
        std::thread([guest] {
            while (true)
            {
                Sleep(1);
                UpdateKeTimeStampBundle(guest);
            }
        }).detach();
    }

    void InitExLoadedCommandLine(uint32_t guest)
    {
        constexpr char commandLine[] = "\"default.xex\"";
        memcpy(g_memory.Translate(guest), commandLine, sizeof(commandLine));
    }

    void InitVdGpuClockInMHz(uint32_t guest)
    {
        StoreBE32(guest, 500);
    }

    std::vector<uint8_t> g_xexHeader;

    void InitXexExecutableModuleHandle(uint32_t guest)
    {
        // The variable holds the module handle, a pointer to the LDR_DATA_TABLE_ENTRY whose
        // +0x58 field points at a copy of the XEX header (read by RtlImageXexHeaderField).
        uint32_t header = GuestAlloc(g_xexHeader.size());
        memcpy(g_memory.Translate(header), g_xexHeader.data(), g_xexHeader.size());

        uint32_t ldrEntry = GuestAlloc(0x100);
        StoreBE32(ldrEntry + 0x58, header);
        StoreBE32(guest, ldrEntry);
    }

    struct KernelVariable
    {
        uint16_t ordinal;
        const char* name;
        uint32_t size;
        void (*init)(uint32_t guest);
    };

    // xboxkrnl.exe variables imported by this title (ordinals from XenonUtils/xbox/xboxkrnl_table.inc).
    constexpr KernelVariable XBOXKRNL_VARIABLES[] =
    {
        { 0x001B, "ExThreadObjectType", 0x40, nullptr },
        { 0x0059, "KeDebugMonitorData", 0x100, nullptr },
        { 0x00AD, "KeTimeStampBundle", 0x18, InitKeTimeStampBundle },
        { 0x0158, "XboxKrnlVersion", 0x8, InitXboxKrnlVersion },
        { 0x0193, "XexExecutableModuleHandle", 0x4, InitXexExecutableModuleHandle },
        { 0x01AE, "ExLoadedCommandLine", 0x400, InitExLoadedCommandLine },
        { 0x01BE, "VdGlobalDevice", 0x4, nullptr },
        { 0x01C0, "VdGpuClockInMHz", 0x4, InitVdGpuClockInMHz },
        { 0x01C1, "VdHSIOCalibrationLock", 0x10, nullptr },
        { 0x0266, "KeCertMonitorData", 0x100, nullptr },
    };

    void ResolveImports(const std::vector<uint8_t>& xex)
    {
        uint32_t header = FindOptionalHeader(xex, XEX_HEADER_IMPORT_LIBRARIES);
        if (header == 0)
            return;

        uint32_t stringTableSize = ReadBE32(xex, header + 4);
        uint32_t libraryCount = ReadBE32(xex, header + 8);

        std::vector<std::string_view> names;
        size_t stringOffset = header + 12;
        for (uint32_t i = 0; i < libraryCount; i++)
        {
            names.emplace_back(reinterpret_cast<const char*>(xex.data() + stringOffset));
            stringOffset += (names.back().size() + 1 + 3) & ~3;
        }

        size_t library = header + 12 + stringTableSize;
        for (uint32_t i = 0; i < libraryCount; i++)
        {
            uint32_t librarySize = ReadBE32(xex, library);
            std::string_view libraryName = names[ReadBE16(xex, library + 0x24)];
            uint16_t recordCount = ReadBE16(xex, library + 0x26);

            uint32_t thunks = 0, variables = 0;
            for (uint16_t r = 0; r < recordCount; r++)
            {
                uint32_t slot = ReadBE32(xex, library + 0x28 + r * 4);
                if (slot >= PPC_CODE_BASE)
                    continue; // thunk, already replaced by XenonRecomp

                // Each function import is an IAT slot followed by its thunk record; a slot not
                // followed by a thunk is a variable import.
                uint32_t next = r + 1 < recordCount ? ReadBE32(xex, library + 0x28 + (r + 1) * 4) : 0;
                if (next >= PPC_CODE_BASE)
                {
                    StoreBE32(slot, next);
                    thunks++;
                    continue;
                }

                // xexdump's image keeps the original import record here, byte-swapped by
                // XenonUtils (low 16 bits = ordinal).
                uint16_t ordinal = uint16_t(*reinterpret_cast<uint32_t*>(g_memory.Translate(slot)));

                const KernelVariable* variable = nullptr;
                if (libraryName == "xboxkrnl.exe")
                {
                    for (auto& v : XBOXKRNL_VARIABLES)
                        variable = v.ordinal == ordinal ? &v : variable;
                }

                uint32_t guest = GuestAlloc(variable ? variable->size : 0x1000);
                if (variable && variable->init)
                    variable->init(guest);

                StoreBE32(slot, guest);
                variables++;

                Log("[loader] variable import %.*s!%s (ordinal 0x%X): IAT 0x%08X -> 0x%08X",
                    int(libraryName.size()), libraryName.data(), variable ? variable->name : "(unknown, zeroed 4 KB)", ordinal, slot, guest);
            }

            Log("[loader] %.*s: %u function imports, %u variable imports",
                int(libraryName.size()), libraryName.data(), thunks, variables);

            library += librarySize;
        }
    }

    // Sorted guest starts of the generated functions (PPCFuncMappings is not sorted).
    std::vector<uint32_t> g_functionStarts;

    constexpr uint32_t PPC_BLR = 0x4E800020;
    constexpr uint32_t PPC_BCTR = 0x4E800420;

    bool IsCode(uint32_t address)
    {
        return address >= PPC_CODE_BASE && address < PPC_CODE_BASE + PPC_CODE_SIZE;
    }

    bool IsMapped(uint32_t address)
    {
        return std::binary_search(g_functionStarts.begin(), g_functionStarts.end(), address);
    }

    // Nearest generated function that starts at or before the address (0 if none).
    uint32_t ContainingFunction(uint32_t address)
    {
        auto it = std::upper_bound(g_functionStarts.begin(), g_functionStarts.end(), address);
        return it == g_functionStarts.begin() ? 0 : *(it - 1);
    }

    // The word before the address ends a block: blr, bctr, unconditional b, or zero padding.
    // Code that starts right after one of these and has no function of its own is usually a
    // function that the recompiler config swallowed into the previous one.
    bool FollowsTerminator(uint32_t address)
    {
        uint32_t prev = LoadBE32(address - 4);
        return prev == PPC_BLR || prev == PPC_BCTR || prev == 0 || (prev >> 26 == 18 && (prev & 3) == 0);
    }

    PPC_FUNC(UnmappedIndirectCall)
    {
        uint32_t target = GuestCTR(ctx);
        if (!IsCode(target) || (target & 3))
        {
            char caller[512];
            Fatal("indirect call to unmapped guest address 0x%08X (ctr), lr=0x%08X in %s: not an instruction "
                  "address (bad or uninitialized function pointer)", target, GuestLR(ctx),
                  DescribeGuestCode(GuestLR(ctx), caller, sizeof(caller)));
        }

        char inside[512], caller[512];
        Fatal("indirect call to unmapped guest address 0x%08X (ctr), lr=0x%08X in %s: inside generated function "
              "%s%s. Run `python tools/find_swallowed.py --suggest`, split the range in "
              "config/Crash.toml and regenerate (see README, \"Funções engolidas\")",
              target, GuestLR(ctx), DescribeGuestCode(GuestLR(ctx), caller, sizeof(caller)),
              DescribeGuestCode(target, inside, sizeof(inside)),
              FollowsTerminator(target) ? ", right after blr/b/padding: a function swallowed by the recompiler config" : "");
    }

    // Startup check: 32-bit words in the data sections that point at code right after a block
    // terminator but have no generated function. Each one is an indirect call that would end in
    // UnmappedIndirectCall. Jump-table labels are not in the data sections (tables sit after their
    // bctr in .text); C++/SEH scope tables point mid-function, so they are filtered by
    // FollowsTerminator plus a check that no branch in the containing function targets the word.
    void CheckDataCodePointers()
    {
        const uint32_t peOffset = LoadBE32(PPC_IMAGE_BASE + 0x3C);
        const uint32_t pe = PPC_IMAGE_BASE + __builtin_bswap32(peOffset);
        auto le16 = [](uint32_t a) { uint16_t v; memcpy(&v, g_memory.Translate(a), 2); return v; };
        auto le32 = [](uint32_t a) { return __builtin_bswap32(LoadBE32(a)); };
        const uint32_t sectionCount = le16(pe + 6);
        const uint32_t sectionTable = pe + 24 + le16(pe + 20);

        size_t pointers = 0;
        std::vector<std::pair<uint32_t, uint32_t>> suspicious; // (target, where)
        for (uint32_t i = 0; i < sectionCount; i++)
        {
            uint32_t header = sectionTable + 40 * i;
            char name[9]{};
            memcpy(name, g_memory.Translate(header), 8);
            uint32_t size = le32(header + 8);
            uint32_t start = PPC_IMAGE_BASE + le32(header + 12);
            uint32_t flags = le32(header + 36);
            if ((flags & 0x20) || !strcmp(name, ".pdata") || !strcmp(name, ".reloc") || !strcmp(name, ".idata") || !strcmp(name, ".XBLD"))
                continue;
            uint32_t end = std::min<uint64_t>(uint64_t(start) + size, PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
            for (uint32_t a = start; a + 4 <= end; a += 4)
            {
                uint32_t target = LoadBE32(a);
                if (!IsCode(target) || (target & 3))
                    continue;
                pointers++;
                if (IsMapped(target) || !FollowsTerminator(target))
                    continue;

                // Skip internal labels: a branch inside the containing function targets the word.
                uint32_t container = ContainingFunction(target);
                auto next = std::upper_bound(g_functionStarts.begin(), g_functionStarts.end(), target);
                uint32_t limit = next == g_functionStarts.end() ? target + 0x1000 : *next;
                bool label = false;
                for (uint32_t pc = container; pc < limit && !label; pc += 4)
                {
                    uint32_t insn = LoadBE32(pc);
                    int32_t displacement = 0;
                    if (insn >> 26 == 18 && !(insn & 3))
                        displacement = int32_t(insn << 6) >> 6 & ~3;
                    else if (insn >> 26 == 16 && !(insn & 3))
                        displacement = int16_t(insn & 0xFFFC);
                    else
                        continue;
                    label = pc + displacement == target;
                }
                if (!label)
                    suspicious.emplace_back(target, a);
            }
        }

        Log("[loader] %zu data words point into code; %zu point at unmapped code after blr/b/padding%s",
            pointers, suspicious.size(), suspicious.empty() ? "" : " (possible functions swallowed by the config, see tools/find_swallowed.py):");
        for (auto [target, where] : suspicious)
        {
            char inside[512];
            Log("[loader]   0x%08X (in %s), referenced from 0x%08X", target, DescribeGuestCode(target, inside, sizeof(inside)), where);
        }
    }
}

bool LoadXex(const std::filesystem::path& xexPath, const std::filesystem::path& imagePath, XexModule& module)
{
    std::vector<uint8_t> xex = ReadFile(xexPath);
    if (xex.size() < 0x18 || memcmp(xex.data(), "XEX2", 4) != 0)
    {
        Log("[loader] %s is missing or not an XEX2 file", xexPath.string().c_str());
        return false;
    }

    // The image: decrypted from the XEX itself (the default), or a pre-made image.bin (--image).
    std::vector<uint8_t> image;
    if (imagePath.empty())
    {
        uint32_t base = 0;
        std::string error;
        if (!xex_image::Load(xex, image, base, error))
        {
            Log("[loader] %s: %s", xexPath.string().c_str(), error.c_str());
            return false;
        }
        if (base != PPC_IMAGE_BASE || image.size() != PPC_IMAGE_SIZE)
        {
            Log("[loader] %s: image 0x%08X + 0x%zX, the recompiled code expects 0x%08llX + 0x%llX: is this the same default.xex?",
                xexPath.string().c_str(), base, image.size(), PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
            return false;
        }
        Log("[loader] image decrypted from %s", xexPath.string().c_str());
    }
    else
    {
        image = ReadFile(imagePath);
        if (image.size() != PPC_IMAGE_SIZE)
        {
            Log("[loader] %s: expected 0x%llX bytes, got 0x%zX (regenerate it with tools/xexdump)",
                imagePath.string().c_str(), PPC_IMAGE_SIZE, image.size());
            return false;
        }
    }

    if (!g_memory.Commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE))
        return false;

    memcpy(g_memory.Translate(PPC_IMAGE_BASE), image.data(), image.size());

    uint32_t peDataOffset = ReadBE32(xex, 0x08);
    g_xexHeader.assign(xex.begin(), xex.begin() + peDataOffset);

    module.entryPoint = FindOptionalHeader(xex, XEX_HEADER_ENTRY_POINT);
    module.defaultStackSize = FindOptionalHeader(xex, XEX_HEADER_DEFAULT_STACK_SIZE);
    if (uint32_t executionInfo = FindOptionalHeader(xex, XEX_HEADER_EXECUTION_INFO); executionInfo && executionInfo + 0x10 <= xex.size())
        module.titleId = ReadBE32(xex, executionInfo + 0x0C); // media id, version, base version, title id, ...
    g_titleId = module.titleId;

    Log("[loader] image 0x%08llX-0x%08llX, entry point 0x%08X, default stack size 0x%X, title id %08X",
        PPC_IMAGE_BASE, PPC_IMAGE_BASE + PPC_IMAGE_SIZE, module.entryPoint, module.defaultStackSize, module.titleId);

    ResolveImports(xex);
    return true;
}

void InitFunctionTable()
{
    // One host pointer per guest instruction (4 bytes of code -> 8 bytes of table), right after
    // the image; see PPC_LOOKUP_FUNC.
    constexpr uint64_t tableBase = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    constexpr uint64_t tableSize = PPC_CODE_SIZE * 2;

    if (!g_memory.Commit(uint32_t(tableBase), tableSize))
        Fatal("could not commit the function table");

    auto* table = reinterpret_cast<PPCFunc**>(g_memory.Translate(uint32_t(tableBase)));
    for (size_t i = 0; i < tableSize / sizeof(PPCFunc*); i++)
        table[i] = UnmappedIndirectCall;

    size_t count = 0;
    for (; PPCFuncMappings[count].host != nullptr; count++)
    {
        PPC_LOOKUP_FUNC(g_memory.base, PPCFuncMappings[count].guest) = PPCFuncMappings[count].host;
        g_functionStarts.push_back(PPCFuncMappings[count].guest);
    }
    std::sort(g_functionStarts.begin(), g_functionStarts.end());

    Log("[loader] function table at 0x%08llX: %zu functions", tableBase, count);
    CheckDataCodePointers();
}

uint32_t CurrentTitleId()
{
    return g_titleId;
}
