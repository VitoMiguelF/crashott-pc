#pragma once

// CRASH_GPU_VENDOR: pretend the GPU is from another vendor, to test on one PC what a player with
// another card sees (DLSS greyed out and skipped in the options, the startup fallback, the
// installer leaving the DLSS files out). A PCI vendor id in hex ("0x1002", "10DE") or a name:
// nvidia, amd, intel. Read by the runtime (gpu/upscaler.cpp), the launcher and the installer.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace settings
{
    constexpr uint32_t VENDOR_NVIDIA = 0x10DE, VENDOR_AMD = 0x1002, VENDOR_INTEL = 0x8086;

    // The forced vendor id, or 0 when CRASH_GPU_VENDOR is not set (or not understood).
    inline uint32_t ForcedGpuVendor()
    {
        const char* text = getenv("CRASH_GPU_VENDOR");
        if (!text || !*text)
            return 0;
        std::string name(text);
        for (char& c : name)
            c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if (name == "nvidia")
            return VENDOR_NVIDIA;
        if (name == "amd" || name == "ati")
            return VENDOR_AMD;
        if (name == "intel")
            return VENDOR_INTEL;
        return uint32_t(strtoul(text, nullptr, 16));
    }

    inline const char* GpuVendorName(uint32_t vendor)
    {
        switch (vendor)
        {
        case VENDOR_NVIDIA: return "NVIDIA";
        case VENDOR_AMD: return "AMD";
        case VENDOR_INTEL: return "Intel";
        case 0: return "?";
        default: return "other";
        }
    }
}
