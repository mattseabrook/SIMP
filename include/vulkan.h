#ifndef SIMP_VULKAN_H
#define SIMP_VULKAN_H

#define VK_USE_PLATFORM_WIN32_KHR

#include <cstdint>
#include <vector>

#include <windows.h>

#include <vulkan/vulkan.h>

struct DecodedImage;

struct VulkanContext
{
    HWND hwnd = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkFramebuffer> overlayFramebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkRenderPass overlayRenderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    std::vector<VkFence> imagesInFlight;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::uint32_t graphicsQueueFamily = 0;
    std::uint32_t presentQueueFamily = 0;
    std::uint32_t pendingWidth = 0;
    std::uint32_t pendingHeight = 0;
    std::size_t currentFrame = 0;
    VkImage canvasImage = VK_NULL_HANDLE;
    VkDeviceMemory canvasImageMemory = VK_NULL_HANDLE;
    std::uint32_t canvasImageWidth = 0;
    std::uint32_t canvasImageHeight = 0;
    bool framebufferResized = false;
    bool initialized = false;
};

extern VulkanContext vkCtx;

void initializeVulkan(HWND hwnd);
void resizeVulkan(std::uint32_t width, std::uint32_t height);
void setVulkanCanvasImage(const DecodedImage& image);
void clearVulkanCanvasImage();
void renderFrameVk();
void cleanupVulkan();

#endif
