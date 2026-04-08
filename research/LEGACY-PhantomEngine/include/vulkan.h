#ifndef VULKAN_H
#define VULKAN_H

#include <vector>
#include <cstdint>
#include <vulkan/vulkan.h>

//
// Context for Vulkan rendering
//
struct VulkanContext
{
    VkInstance instance{};
    VkPhysicalDevice physicalDevice{};
    VkDevice device{};
    VkQueue graphicsQueue{};
    VkCommandPool commandPool{};
    VkCommandBuffer frameCommandBuffer{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    std::vector<VkImage> swapchainImages;
    VkFormat swapchainFormat{};
    VkExtent2D swapchainExtent{};
    VkSemaphore imageAvailableSemaphore{};
    VkSemaphore renderFinishedSemaphore{};
    VkFence inFlightFence{};
    VkImage textureImage{};
    VkDeviceMemory textureImageMemory{};
    VkImageView textureImageView{};
    VkSampler textureSampler{};
    void *mappedTextureData{};
    VkDeviceSize textureRowPitch{};

    // GPU raycast resources (optional; used when raycast is enabled and renderMode != CPU)
    VkImage raycastImage{};
    VkDeviceMemory raycastImageMemory{};
    VkImageView raycastImageView{};
    VkImageLayout raycastImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkDescriptorSetLayout raycastDescSetLayout{};
    VkPipelineLayout raycastPipelineLayout{};
    VkPipeline raycastPipeline{};
    VkDescriptorPool raycastDescPool{};
    VkDescriptorSet raycastDescSet{};

    VkBuffer tileMapBuffer{};
    VkDeviceMemory tileMapBufferMemory{};
    void *mappedTileMapData{};
    VkDeviceSize tileMapBufferSize{};
    uint32_t lastMapWidth = 0;
    uint32_t lastMapHeight = 0;
    uint64_t lastTileMapHash = 0;
    bool hasTileMapHash = false;
    uint64_t lastExploredHash = 0;
    bool hasExploredHash = false;
    std::vector<uint8_t> tileMapUploadCache;

    VkBuffer edgeOffsetsBuffer{};
    VkDeviceMemory edgeOffsetsBufferMemory{};
    void *mappedEdgeOffsetsData{};
    VkDeviceSize edgeOffsetsBufferSize{};
    uint64_t lastEdgeHash = 0;
    bool hasEdgeHash = false;
    bool edgeOffsetsDirty = true;
    size_t lastEdgeCount = 0;
    std::vector<uint32_t> edgeOffsetsUploadCache;

    VkBuffer megatexBuffer{};
    VkDeviceMemory megatexBufferMemory{};
    void *mappedMegatexData{};
    VkDeviceSize megatexBufferSize{};
    uint64_t lastMegatexVersion = 0;
    bool hasMegatexVersion = false;
    std::vector<uint32_t> megatexUploadCache;

    uint32_t graphicsQueueFamily = 0;
    std::vector<uint8_t> rowBuffer;
    std::vector<uint8_t> previousFrameData;
    bool forceFullUpdate = true;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
};

extern VulkanContext vkCtx;

//==========================================================

// Function prototypes

void initializeVulkan();
void resizeVulkanTexture(uint32_t width, uint32_t height);
void recreateSwapchain(uint32_t width, uint32_t height);
void *mapVulkanTexture();
void unmapVulkanTexture();
void renderFrameVk();
void renderFrameRaycastVk();
void renderFrameMenuVk();
void renderSimpleText(uint8_t* framebuffer, size_t pitch, int width, int height, 
					  const char* text, int x, int y, uint32_t color);
void presentFrame();
void cleanupVulkan();

#endif
