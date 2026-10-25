#include "vulkan_loader.h"

#include "../platform/platform.h"

#define CRASH_VK_DEFINE(name) PFN_##name name = nullptr;
namespace gpu::vk
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    CRASH_VK_GLOBAL_FUNCTIONS(CRASH_VK_DEFINE)
    CRASH_VK_INSTANCE_FUNCTIONS(CRASH_VK_DEFINE)
    CRASH_VK_DEVICE_FUNCTIONS(CRASH_VK_DEFINE)

    bool LoadLoader()
    {
        if (vkGetInstanceProcAddr)
            return true;
#ifdef _WIN32
        void* library = plat::OpenLibrary("vulkan-1.dll");
#elif defined(__ANDROID__)
        void* library = plat::OpenLibrary("libvulkan.so");
#else
        void* library = plat::OpenLibrary("libvulkan.so.1");
#endif
        if (!library)
            return false;
        vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::FindSymbol(library, "vkGetInstanceProcAddr"));
        if (!vkGetInstanceProcAddr)
            return false;
#define CRASH_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
        CRASH_VK_GLOBAL_FUNCTIONS(CRASH_VK_LOAD_GLOBAL)
#undef CRASH_VK_LOAD_GLOBAL
        return vkCreateInstance != nullptr;
    }

    void LoadInstanceFunctions(VkInstance instance)
    {
#define CRASH_VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
        CRASH_VK_INSTANCE_FUNCTIONS(CRASH_VK_LOAD_INSTANCE)
#undef CRASH_VK_LOAD_INSTANCE
    }

    void LoadDeviceFunctions(VkDevice device)
    {
#define CRASH_VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
        CRASH_VK_DEVICE_FUNCTIONS(CRASH_VK_LOAD_DEVICE)
#undef CRASH_VK_LOAD_DEVICE
    }
}
#undef CRASH_VK_DEFINE
