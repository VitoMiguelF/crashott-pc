// Vulkan backend of the rendering hardware interface (rhi.h). Work in progress.

#include "rhi.h"
#include "../diagnostics.h"

namespace gpu::rhi
{
    Device* CreateVulkan(const DeviceInfo&)
    {
        Log("[gpu] the Vulkan renderer is not built yet");
        return nullptr;
    }

    bool TestCompileVulkan(Stage, const std::string&, const char*, Layout, const std::filesystem::path&)
    {
        return false;
    }
}
