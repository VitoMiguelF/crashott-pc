#pragma once

// Vulkan entry points of the Vulkan backend (rhi_vulkan.cpp), loaded at run time from the system's
// loader (vulkan-1.dll; libvulkan.so.1 on Linux later): no import library, so the exe still starts
// where Vulkan is missing and the renderer falls back to Direct3D 12.

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

// Functions of the loader and of the instance (before / without a device).
#define CRASH_VK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)

#define CRASH_VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateWin32SurfaceKHR) \
    X(vkCreateDebugUtilsMessengerEXT) \
    X(vkDestroyDebugUtilsMessengerEXT) \
    X(vkSetDebugUtilsObjectNameEXT)

#define CRASH_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit2) \
    X(vkQueueWaitIdle) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkWaitSemaphores) \
    X(vkGetSemaphoreCounterValue) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) \
    X(vkDestroyPipelineCache) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkGetQueryPoolResults) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushDescriptorSetKHR) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilReference) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp2) \
    X(vkCmdBeginDebugUtilsLabelEXT) \
    X(vkCmdEndDebugUtilsLabelEXT)

#define CRASH_VK_DECLARE(name) extern PFN_##name name;
namespace gpu::vk
{
    extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    CRASH_VK_GLOBAL_FUNCTIONS(CRASH_VK_DECLARE)
    CRASH_VK_INSTANCE_FUNCTIONS(CRASH_VK_DECLARE)
    CRASH_VK_DEVICE_FUNCTIONS(CRASH_VK_DECLARE)

    // Loads the Vulkan loader and its global functions; false if Vulkan isn't installed.
    bool LoadLoader();
    void LoadInstanceFunctions(VkInstance instance);
    void LoadDeviceFunctions(VkDevice device);
}
#undef CRASH_VK_DECLARE
