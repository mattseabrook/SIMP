#include "vulkan.h"

#include "app.h"
#include "app_version.h"
#include "editor_theme.h"
#include "cels.h"
#include "window.h"
#include "system.h"
#include "ui_shaders.generated.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>
#include <deque>

#include <vulkan/vulkan_win32.h>

namespace
{
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
    std::vector<VkFramebuffer> overlayFramebuffers;
    VkRenderPass overlayRenderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceProperties deviceProperties{};
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::uint32_t graphicsQueueFamily = 0;
    std::uint32_t presentQueueFamily = 0;
    std::uint32_t pendingWidth = 0;
    std::uint32_t pendingHeight = 0;
    std::size_t currentFrame = 0;
    VkPipeline uiPipeline = VK_NULL_HANDLE;
    VkPipelineLayout uiPipelineLayout = VK_NULL_HANDLE;
    VkPipeline paintPipeline = VK_NULL_HANDLE;
    VkPipelineLayout paintLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout paintSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool paintPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout canvasSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout canvasLayout = VK_NULL_HANDLE;
    VkPipeline canvasPipeline = VK_NULL_HANDLE;
    VkSampler canvasSampler = VK_NULL_HANDLE;
    bool computePainting = false;
    bool framebufferResized = false;
    bool initialized = false;
};

VulkanContext vkCtx;
VkPhysicalDeviceProperties g_publishedProperties{};
bool g_publishedCompute = false;
} // namespace

namespace
{
constexpr std::size_t kMaxFramesInFlight = 2;
constexpr std::array<const char *, 1> kRequiredDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

struct QueueFamilyIndices
{
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    bool complete() const
    {
        return graphics.has_value() && present.has_value();
    }
};

struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

void throwIfFailed(VkResult result, const char *message)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(message);
}

std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    const VkPhysicalDeviceMemoryProperties &memoryProperties = vkCtx.memoryProperties;

    for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 &&
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }

    throw std::runtime_error("Failed to find a suitable Vulkan memory type.");
}

void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer,
                  VkDeviceMemory &memory)
{
    VkBufferCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size = size;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    throwIfFailed(vkCreateBuffer(vkCtx.device, &createInfo, nullptr, &buffer), "Failed to create a Vulkan buffer.");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(vkCtx.device, buffer, &requirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);

    throwIfFailed(vkAllocateMemory(vkCtx.device, &allocateInfo, nullptr, &memory),
                  "Failed to allocate Vulkan buffer memory.");
    throwIfFailed(vkBindBufferMemory(vkCtx.device, buffer, memory, 0), "Failed to bind Vulkan buffer memory.");
}

struct HostBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize capacity = 0;
};
struct FrameResources
{
    HostBuffer ui, upload;
};
std::array<FrameResources, kMaxFramesInFlight> g_frames;
struct CelTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet sampled = VK_NULL_HANDLE, paint = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0;
    bool visible = true;
};
std::vector<CelTexture> g_celTextures;
std::uint32_t g_activeCel = 0;
struct PendingUpload
{
    std::vector<std::uint8_t> pixels;
    bool paint = false;
    std::uint32_t color = 0, cel = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
};
std::vector<PendingUpload> g_pendingUploads;

void destroyHostBuffer(HostBuffer &buffer)
{
    if (buffer.mapped)
        vkUnmapMemory(vkCtx.device, buffer.memory);
    if (buffer.buffer)
        vkDestroyBuffer(vkCtx.device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(vkCtx.device, buffer.memory, nullptr);
    buffer = {};
}
void ensureHostBuffer(HostBuffer &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (buffer.capacity >= bytes)
        return;
    // The caller has waited for this flight slot's fence before reaching here.
    destroyHostBuffer(buffer);
    buffer.capacity = std::max<VkDeviceSize>(65536, bytes + bytes / 4);
    createBuffer(buffer.capacity, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 buffer.buffer, buffer.memory);
    throwIfFailed(vkMapMemory(vkCtx.device, buffer.memory, 0, buffer.capacity, 0, &buffer.mapped),
                  "Cannot map frame upload buffer.");
}

void createImage(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage &image,
                 VkDeviceMemory &memory)
{
    VkImageCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.extent.width = width;
    createInfo.extent.height = height;
    createInfo.extent.depth = 1;
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.format = format;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    createInfo.usage = usage;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    throwIfFailed(vkCreateImage(vkCtx.device, &createInfo, nullptr, &image), "Failed to create a Vulkan image.");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(vkCtx.device, image, &requirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    throwIfFailed(vkAllocateMemory(vkCtx.device, &allocateInfo, nullptr, &memory),
                  "Failed to allocate Vulkan image memory.");
    throwIfFailed(vkBindImageMemory(vkCtx.device, image, memory, 0), "Failed to bind Vulkan image memory.");
}

VkCommandBuffer beginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandPool = vkCtx.commandPool;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    throwIfFailed(vkAllocateCommandBuffers(vkCtx.device, &allocateInfo, &commandBuffer),
                  "Failed to allocate a Vulkan upload command buffer.");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    throwIfFailed(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin a Vulkan upload command buffer.");
    return commandBuffer;
}

void endSingleTimeCommands(VkCommandBuffer commandBuffer)
{
    throwIfFailed(vkEndCommandBuffer(commandBuffer), "Failed to end a Vulkan upload command buffer.");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    throwIfFailed(vkQueueSubmit(vkCtx.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE),
                  "Failed to submit Vulkan upload work.");
    throwIfFailed(vkQueueWaitIdle(vkCtx.graphicsQueue), "Failed to wait for Vulkan upload work.");
    vkFreeCommandBuffers(vkCtx.device, vkCtx.commandPool, 1, &commandBuffer);
}

void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                           VkImageLayout newLayout, VkPipelineStageFlags sourceStage,
                           VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess,
                           VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void recordCanvasUpload(VkCommandBuffer command)
{
    if (g_pendingUploads.empty())
        return;
    VkDeviceSize bytes = 0;
    for (const auto &pending : g_pendingUploads)
        if (!pending.paint)
            bytes += VkDeviceSize(pending.width) * pending.height * 4;
    auto &buffer = g_frames[vkCtx.currentFrame].upload;
    if (bytes)
        ensureHostBuffer(buffer, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkDeviceSize offset = 0;
    for (const auto &pending : g_pendingUploads)
    {
        const auto &target = g_celTextures[pending.cel];
        if (pending.paint)
        {
            transitionImageLayout(command, target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            struct Push
            {
                std::int32_t rect[4];
                float color[4];
            };
            const auto color = pending.color;
            const Push push{{int(pending.x), int(pending.y), int(pending.width), int(pending.height)},
                            {float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255,
                             float(color & 255) / 255, float(color >> 24) / 255}};
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, vkCtx.paintPipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, vkCtx.paintLayout, 0, 1, &target.paint, 0,
                                    nullptr);
            vkCmdPushConstants(command, vkCtx.paintLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(command, (pending.width + 15) / 16, (pending.height + 7) / 8, 1);
            transitionImageLayout(command, target.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            continue;
        }
        const auto size = VkDeviceSize(pending.width) * pending.height * 4;
        std::memcpy(static_cast<std::uint8_t *>(buffer.mapped) + offset, pending.pixels.data(),
                    std::size_t(size));
        transitionImageLayout(command, target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {int32_t(pending.x), int32_t(pending.y), 0};
        region.imageExtent = {pending.width, pending.height, 1};
        vkCmdCopyBufferToImage(command, buffer.buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transitionImageLayout(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT);
        offset += size;
    }
    g_pendingUploads.clear();
}

void copyBufferToImage(VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height)
{
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    transitionImageLayout(commandBuffer, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionImageLayout(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    endSingleTimeCommands(commandBuffer);
}

void destroyCanvasTexture()
{
    for (auto &cel : g_celTextures)
    {
        if (cel.view)
            vkDestroyImageView(vkCtx.device, cel.view, nullptr);
        if (cel.image)
            vkDestroyImage(vkCtx.device, cel.image, nullptr);
        if (cel.memory)
            vkFreeMemory(vkCtx.device, cel.memory, nullptr);
    }
    g_celTextures.clear();
    if (vkCtx.paintPool)
        vkDestroyDescriptorPool(vkCtx.device, vkCtx.paintPool, nullptr);
    vkCtx.paintPool = VK_NULL_HANDLE;
}

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device)
{
    QueueFamilyIndices indices;

    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (std::uint32_t index = 0; index < queueFamilyCount; ++index)
    {
        if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            indices.graphics = index;

        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, index, vkCtx.surface, &presentSupported);
        if (presentSupported == VK_TRUE)
            indices.present = index;

        if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupported)
        {
            indices.graphics = index;
            indices.present = index;
            break;
        }
    }

    return indices;
}

bool checkDeviceExtensionSupport(VkPhysicalDevice device)
{
    std::uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    for (const char *requiredExtension : kRequiredDeviceExtensions)
    {
        bool found = false;
        for (const VkExtensionProperties &extension : availableExtensions)
        {
            if (std::strcmp(requiredExtension, extension.extensionName) == 0)
            {
                found = true;
                break;
            }
        }

        if (!found)
            return false;
    }

    return true;
}

SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device)
{
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, vkCtx.surface, &details.capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, vkCtx.surface, &formatCount, nullptr);
    if (formatCount > 0)
    {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, vkCtx.surface, &formatCount, details.formats.data());
    }

    std::uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, vkCtx.surface, &presentModeCount, nullptr);
    if (presentModeCount > 0)
    {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, vkCtx.surface, &presentModeCount,
                                                  details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats)
{
    for (const VkSurfaceFormatKHR &format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }

    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        return {VK_FORMAT_B8G8R8A8_UNORM, formats[0].colorSpace};
    return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR> &presentModes)
{
    for (const VkPresentModeKHR mode : presentModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR &capabilities)
{
    if (capabilities.currentExtent.width != UINT32_MAX)
        return capabilities.currentExtent;

    RECT clientRect{};
    GetClientRect(vkCtx.hwnd, &clientRect);

    VkExtent2D extent{};
    extent.width = std::clamp(static_cast<std::uint32_t>(std::max(1L, clientRect.right - clientRect.left)),
                              capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(static_cast<std::uint32_t>(std::max(1L, clientRect.bottom - clientRect.top)),
                               capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

bool isPhysicalDeviceSuitable(VkPhysicalDevice device)
{
    const QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.complete())
        return false;

    if (!checkDeviceExtensionSupport(device))
        return false;

    const SwapchainSupportDetails swapchainSupport = querySwapchainSupport(device);
    return !swapchainSupport.formats.empty() && !swapchainSupport.presentModes.empty();
}

void createInstance()
{
    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "SIMP " APP_VERSION_STRING;
    applicationInfo.applicationVersion =
        VK_MAKE_VERSION(APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_BUILD & 0xfff);
    applicationInfo.pEngineName = "SIMP";
    applicationInfo.engineVersion = applicationInfo.applicationVersion;
    applicationInfo.apiVersion = VK_API_VERSION_1_1;

    constexpr std::array<const char *, 2> extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                                        VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    throwIfFailed(vkCreateInstance(&createInfo, nullptr, &vkCtx.instance), "Failed to create the Vulkan instance.");
}

void createSurface()
{
    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = GetModuleHandleW(nullptr);
    createInfo.hwnd = vkCtx.hwnd;

    throwIfFailed(vkCreateWin32SurfaceKHR(vkCtx.instance, &createInfo, nullptr, &vkCtx.surface),
                  "Failed to create the Vulkan Win32 surface.");
}

void pickPhysicalDevice()
{
    uint32_t count = 0;
    throwIfFailed(vkEnumeratePhysicalDevices(vkCtx.instance, &count, nullptr), "Cannot enumerate Vulkan GPUs.");
    std::vector<VkPhysicalDevice> devices(count);
    throwIfFailed(vkEnumeratePhysicalDevices(vkCtx.instance, &count, devices.data()), "Cannot enumerate Vulkan GPUs.");
    uint64_t best = 0;
    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.apiVersion < VK_API_VERSION_1_1 || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            !isPhysicalDeviceSuitable(device))
            continue;
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_B8G8R8A8_UNORM, &format);
        if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            continue;
        const auto support = querySwapchainSupport(device);
        const auto surfaceFormat = chooseSurfaceFormat(support.formats);
        vkGetPhysicalDeviceFormatProperties(device, surfaceFormat.format, &format);
        if ((format.optimalTilingFeatures &
             (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) !=
            (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
            continue;
        if (!(support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            continue;
        const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        uint64_t score = discrete ? 2000000 : 1000000;
        if (discrete && props.vendorID == 0x10DE)
            score += 2000000;
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(device, &memory);
        uint64_t localBytes = 0;
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
            if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                localBytes += memory.memoryHeaps[i].size;
        score += std::min<uint64_t>(500000, localBytes / (1024 * 1024));
        score += props.limits.maxImageDimension2D;
        if (score > best)
        {
            best = score;
            vkCtx.physicalDevice = device;
            vkCtx.deviceProperties = props;
            vkCtx.memoryProperties = memory;
        }
    }
    if (!vkCtx.physicalDevice)
        throw std::runtime_error("No hardware Vulkan GPU supports the required canvas and presentation operations.");
    const std::string log = std::string("SIMP Vulkan GPU: ") + vkCtx.deviceProperties.deviceName + "\n";
    OutputDebugStringA(log.c_str());
}

void createLogicalDevice()
{
    const QueueFamilyIndices indices = findQueueFamilies(vkCtx.physicalDevice);
    vkCtx.graphicsQueueFamily = indices.graphics.value();
    vkCtx.presentQueueFamily = indices.present.value();

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    const float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo graphicsQueueInfo{};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = vkCtx.graphicsQueueFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(graphicsQueueInfo);

    if (vkCtx.presentQueueFamily != vkCtx.graphicsQueueFamily)
    {
        VkDeviceQueueCreateInfo presentQueueInfo{};
        presentQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        presentQueueInfo.queueFamilyIndex = vkCtx.presentQueueFamily;
        presentQueueInfo.queueCount = 1;
        presentQueueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(presentQueueInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(vkCtx.physicalDevice, &available);
    std::uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(vkCtx.physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(vkCtx.physicalDevice, nullptr, &extensionCount, extensions.data());
    const bool flags2 = std::any_of(extensions.begin(), extensions.end(), [](const auto &e) {
        return std::strcmp(e.extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) == 0;
    });
    if (flags2 && available.shaderStorageImageWriteWithoutFormat)
    {
        VkFormatProperties3 properties3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        properties2.pNext = &properties3;
        vkGetPhysicalDeviceFormatProperties2(vkCtx.physicalDevice, VK_FORMAT_B8G8R8A8_UNORM, &properties2);
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vkCtx.physicalDevice, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(vkCtx.physicalDevice, &count, queues.data());
        const auto required =
            VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
        vkCtx.computePainting = (properties3.optimalTilingFeatures & required) == required &&
                                (queues[vkCtx.graphicsQueueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT);
        deviceFeatures.shaderStorageImageWriteWithoutFormat = vkCtx.computePainting;
    }
    std::vector<const char *> enabledExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    if (vkCtx.computePainting)
        enabledExtensions.push_back(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = std::uint32_t(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    throwIfFailed(vkCreateDevice(vkCtx.physicalDevice, &createInfo, nullptr, &vkCtx.device),
                  "Failed to create the Vulkan logical device.");

    vkGetDeviceQueue(vkCtx.device, vkCtx.graphicsQueueFamily, 0, &vkCtx.graphicsQueue);
    vkGetDeviceQueue(vkCtx.device, vkCtx.presentQueueFamily, 0, &vkCtx.presentQueue);
}

void createCommandPool()
{
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = vkCtx.graphicsQueueFamily;

    throwIfFailed(vkCreateCommandPool(vkCtx.device, &createInfo, nullptr, &vkCtx.commandPool),
                  "Failed to create the Vulkan command pool.");
}

void createSwapchain()
{
    const SwapchainSupportDetails support = querySwapchainSupport(vkCtx.physicalDevice);
    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
    const VkExtent2D extent = chooseExtent(support.capabilities);

    std::uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
        imageCount = support.capabilities.maxImageCount;

    const std::array<std::uint32_t, 2> queueFamilyIndices = {vkCtx.graphicsQueueFamily, vkCtx.presentQueueFamily};

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = vkCtx.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (vkCtx.graphicsQueueFamily != vkCtx.presentQueueFamily)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilyIndices.size());
        createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(support.capabilities.supportedCompositeAlpha & createInfo.compositeAlpha))
        for (auto alpha : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                           VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (support.capabilities.supportedCompositeAlpha & alpha)
            {
                createInfo.compositeAlpha = alpha;
                break;
            }
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    throwIfFailed(vkCreateSwapchainKHR(vkCtx.device, &createInfo, nullptr, &vkCtx.swapchain),
                  "Failed to create the Vulkan swapchain.");

    vkGetSwapchainImagesKHR(vkCtx.device, vkCtx.swapchain, &imageCount, nullptr);
    vkCtx.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(vkCtx.device, vkCtx.swapchain, &imageCount, vkCtx.swapchainImages.data());
    vkCtx.renderFinishedSemaphores.resize(imageCount);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto &handle : vkCtx.renderFinishedSemaphores)
        throwIfFailed(vkCreateSemaphore(vkCtx.device, &semaphore, nullptr, &handle),
                      "Cannot create presentation semaphore.");

    vkCtx.swapchainFormat = surfaceFormat.format;
    vkCtx.swapchainExtent = extent;
}

void createImageViews()
{
    vkCtx.swapchainImageViews.resize(vkCtx.swapchainImages.size());

    for (std::size_t index = 0; index < vkCtx.swapchainImages.size(); ++index)
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = vkCtx.swapchainImages[index];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = vkCtx.swapchainFormat;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        throwIfFailed(vkCreateImageView(vkCtx.device, &createInfo, nullptr, &vkCtx.swapchainImageViews[index]),
                      "Failed to create a Vulkan swapchain image view.");
    }
}

void createRenderPass()
{
    VkAttachmentDescription attachment{};
    attachment.format = vkCtx.swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    throwIfFailed(vkCreateRenderPass(vkCtx.device, &info, nullptr, &vkCtx.overlayRenderPass),
                  "Cannot create UI render pass.");
}

void createPaintPipeline()
{
    if (!vkCtx.computePainting)
        return;
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    throwIfFailed(vkCreateDescriptorSetLayout(vkCtx.device, &setInfo, nullptr, &vkCtx.paintSetLayout),
                  "Cannot create paint descriptor layout.");
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &vkCtx.paintSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    throwIfFailed(vkCreatePipelineLayout(vkCtx.device, &layout, nullptr, &vkCtx.paintLayout),
                  "Cannot create paint pipeline layout.");
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(kPaintComp);
    shaderInfo.pCode = kPaintComp;
    VkShaderModule shader = VK_NULL_HANDLE;
    throwIfFailed(vkCreateShaderModule(vkCtx.device, &shaderInfo, nullptr, &shader),
                  "Cannot create paint compute shader.");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = vkCtx.paintLayout;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = shader;
    pipeline.stage.pName = "main";
    const auto result =
        vkCreateComputePipelines(vkCtx.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &vkCtx.paintPipeline);
    vkDestroyShaderModule(vkCtx.device, shader, nullptr);
    throwIfFailed(result, "Cannot create paint compute pipeline.");
}

void createGraphicsPipeline(std::span<const std::uint32_t> vertexCode, std::span<const std::uint32_t> fragmentCode,
                            const VkPipelineVertexInputStateCreateInfo &input, VkPipelineLayout layout,
                            VkPipeline &output)
{
    struct ShaderModule
    {
        VkShaderModule handle = VK_NULL_HANDLE;
        explicit ShaderModule(std::span<const std::uint32_t> code)
        {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = code.size_bytes();
            info.pCode = code.data();
            throwIfFailed(vkCreateShaderModule(vkCtx.device, &info, nullptr, &handle), "Cannot create Vulkan shader.");
        }
        ~ShaderModule()
        {
            vkDestroyShaderModule(vkCtx.device, handle, nullptr);
        }
    };
    const ShaderModule vert(vertexCode), frag(fragmentCode);
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert.handle, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag.handle, "main"}};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo sample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &sample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = layout;
    pipeline.renderPass = vkCtx.overlayRenderPass;
    throwIfFailed(vkCreateGraphicsPipelines(vkCtx.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &output),
                  "Cannot create Vulkan graphics pipeline.");
}

void createCanvasPipeline()
{
    if (vkCtx.canvasPipeline)
        return;
    if (!vkCtx.canvasSetLayout)
    {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1;
        info.pBindings = &binding;
        throwIfFailed(vkCreateDescriptorSetLayout(vkCtx.device, &info, nullptr, &vkCtx.canvasSetLayout),
                      "Cannot create cel descriptor layout.");
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &vkCtx.canvasSetLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        throwIfFailed(vkCreatePipelineLayout(vkCtx.device, &layout, nullptr, &vkCtx.canvasLayout),
                      "Cannot create cel pipeline layout.");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        throwIfFailed(vkCreateSampler(vkCtx.device, &sampler, nullptr, &vkCtx.canvasSampler),
                      "Cannot create cel sampler.");
    }
    const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    createGraphicsPipeline(kCanvasVert, kCanvasFrag, input, vkCtx.canvasLayout, vkCtx.canvasPipeline);
}
void drawCels(VkCommandBuffer command, const CanvasRenderData &data)
{
    if (!data.hasImage || g_celTextures.empty())
        return;
    VkViewport viewport{0, 0, float(vkCtx.swapchainExtent.width), float(vkCtx.swapchainExtent.height), 0, 1};
    VkRect2D scissor{{data.viewport.left, data.viewport.top},
                     {std::uint32_t(std::max(0, data.viewport.right - data.viewport.left)),
                      std::uint32_t(std::max(0, data.viewport.bottom - data.viewport.top))}};
    if (!scissor.extent.width || !scissor.extent.height)
        return;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vkCtx.canvasPipeline);
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    struct Push
    {
        float rect[4], surface[2], image[2];
        std::int32_t checker;
    } push{{data.imageX, data.imageY, data.imageWidth, data.imageHeight},
           {viewport.width, viewport.height},
           {float(g_celTextures.front().width), float(g_celTextures.front().height)},
           1};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vkCtx.canvasLayout, 0, 1,
                            &g_celTextures.front().sampled, 0, nullptr);
    vkCmdPushConstants(command, vkCtx.canvasLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 0, 0);
    push.checker = 0;
    vkCmdPushConstants(command, vkCtx.canvasLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    for (const auto &cel : g_celTextures)
        if (cel.visible)
        {
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vkCtx.canvasLayout, 0, 1, &cel.sampled, 0,
                                    nullptr);
            vkCmdDraw(command, 6, 1, 0, 0);
        }
}

void createUiPipeline()
{
    if (vkCtx.uiPipeline)
        return;
    if (!vkCtx.uiPipelineLayout)
    {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        throwIfFailed(vkCreatePipelineLayout(vkCtx.device, &info, nullptr, &vkCtx.uiPipelineLayout),
                      "Cannot create UI pipeline layout.");
    }
    static_assert(sizeof(CanvasOverlayRect) == 8 * sizeof(float));
    VkVertexInputBindingDescription binding{0, sizeof(CanvasOverlayRect), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                       {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 2;
    input.pVertexAttributeDescriptions = attributes;
    createGraphicsPipeline(kUiVert, kUiFrag, input, vkCtx.uiPipelineLayout, vkCtx.uiPipeline);
}

void createFramebuffers()
{
    vkCtx.overlayFramebuffers.resize(vkCtx.swapchainImageViews.size());

    for (std::size_t index = 0; index < vkCtx.swapchainImageViews.size(); ++index)
    {
        VkImageView attachments[] = {vkCtx.swapchainImageViews[index]};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = vkCtx.overlayRenderPass;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = attachments;
        createInfo.width = vkCtx.swapchainExtent.width;
        createInfo.height = vkCtx.swapchainExtent.height;
        createInfo.layers = 1;

        throwIfFailed(vkCreateFramebuffer(vkCtx.device, &createInfo, nullptr, &vkCtx.overlayFramebuffers[index]),
                      "Failed to create a Vulkan overlay framebuffer.");
    }
}

void allocateCommandBuffers()
{
    if (!vkCtx.commandBuffers.empty())
        return;

    vkCtx.commandBuffers.resize(kMaxFramesInFlight);

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = vkCtx.commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<std::uint32_t>(vkCtx.commandBuffers.size());

    throwIfFailed(vkAllocateCommandBuffers(vkCtx.device, &allocateInfo, vkCtx.commandBuffers.data()),
                  "Failed to allocate Vulkan command buffers.");
}

void createSyncObjects()
{
    vkCtx.imageAvailableSemaphores.resize(kMaxFramesInFlight);
    vkCtx.inFlightFences.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (std::size_t index = 0; index < kMaxFramesInFlight; ++index)
    {
        throwIfFailed(vkCreateSemaphore(vkCtx.device, &semaphoreInfo, nullptr, &vkCtx.imageAvailableSemaphores[index]),
                      "Failed to create a Vulkan semaphore.");
        throwIfFailed(vkCreateFence(vkCtx.device, &fenceInfo, nullptr, &vkCtx.inFlightFences[index]),
                      "Failed to create a Vulkan fence.");
    }
}

void cleanupSwapchain(bool keepPipeline = false)
{
    if (vkCtx.canvasPipeline && !keepPipeline)
        vkDestroyPipeline(vkCtx.device, vkCtx.canvasPipeline, nullptr);
    if (!keepPipeline)
        vkCtx.canvasPipeline = VK_NULL_HANDLE;
    if (vkCtx.uiPipeline && !keepPipeline)
        vkDestroyPipeline(vkCtx.device, vkCtx.uiPipeline, nullptr);
    if (!keepPipeline)
        vkCtx.uiPipeline = VK_NULL_HANDLE;
    for (auto semaphore : vkCtx.renderFinishedSemaphores)
        vkDestroySemaphore(vkCtx.device, semaphore, nullptr);
    vkCtx.renderFinishedSemaphores.clear();
    for (VkFramebuffer framebuffer : vkCtx.overlayFramebuffers)
        vkDestroyFramebuffer(vkCtx.device, framebuffer, nullptr);
    vkCtx.overlayFramebuffers.clear();

    if (vkCtx.overlayRenderPass)
    {
        vkDestroyRenderPass(vkCtx.device, vkCtx.overlayRenderPass, nullptr);
        vkCtx.overlayRenderPass = VK_NULL_HANDLE;
    }

    for (VkImageView imageView : vkCtx.swapchainImageViews)
        vkDestroyImageView(vkCtx.device, imageView, nullptr);
    vkCtx.swapchainImageViews.clear();

    if (vkCtx.swapchain)
    {
        vkDestroySwapchainKHR(vkCtx.device, vkCtx.swapchain, nullptr);
        vkCtx.swapchain = VK_NULL_HANDLE;
    }

    vkCtx.swapchainImages.clear();
}

void recreateSwapchain(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    vkDeviceWaitIdle(vkCtx.device);
    const VkFormat oldFormat = vkCtx.swapchainFormat;
    cleanupSwapchain(true);
    createSwapchain();
    if (vkCtx.swapchainFormat != oldFormat && vkCtx.canvasPipeline)
    {
        vkDestroyPipeline(vkCtx.device, vkCtx.canvasPipeline, nullptr);
        vkCtx.canvasPipeline = VK_NULL_HANDLE;
    }
    if (vkCtx.swapchainFormat != oldFormat && vkCtx.uiPipeline)
    {
        vkDestroyPipeline(vkCtx.device, vkCtx.uiPipeline, nullptr);
        vkCtx.uiPipeline = VK_NULL_HANDLE;
    }
    createImageViews();
    createRenderPass();
    createFramebuffers();
    createUiPipeline();
    createCanvasPipeline();
    vkCtx.framebufferResized = false;
}

void drawOverlayRects(VkCommandBuffer commandBuffer, const CanvasRenderData &renderData)
{
    if (renderData.overlays.empty())
        return;
    HostBuffer &buffer = g_frames[vkCtx.currentFrame].ui;
    const VkDeviceSize size = renderData.overlays.size() * sizeof(CanvasOverlayRect);
    ensureHostBuffer(buffer, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    std::memcpy(buffer.mapped, renderData.overlays.data(), static_cast<size_t>(size));
    const VkDeviceSize offset = 0;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkCtx.uiPipeline);
    VkViewport viewport{0, 0, float(vkCtx.swapchainExtent.width), float(vkCtx.swapchainExtent.height), 0, 1};
    VkRect2D scissor{{0, 0}, vkCtx.swapchainExtent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    const float extent[2] = {viewport.width, viewport.height};
    vkCmdPushConstants(commandBuffer, vkCtx.uiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(extent), extent);
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &buffer.buffer, &offset);
    // Preserve painter order for solid UI geometry and translucent ruler glows.
    vkCmdDraw(commandBuffer, 6, static_cast<uint32_t>(renderData.overlays.size()), 0, 0);
}

void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, const CanvasRenderData &renderData)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    throwIfFailed(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin a Vulkan command buffer.");

    recordCanvasUpload(commandBuffer);
    constexpr auto background = EditorTheme::kWorkspace;
    const VkClearValue clear{{{float(background & 255) / 255, float((background >> 8) & 255) / 255,
                               float((background >> 16) & 255) / 255, 1.0f}}};
    VkRenderPassBeginInfo overlayRenderPassInfo{};
    overlayRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    overlayRenderPassInfo.renderPass = vkCtx.overlayRenderPass;
    overlayRenderPassInfo.framebuffer = vkCtx.overlayFramebuffers.at(imageIndex);
    overlayRenderPassInfo.renderArea.extent = vkCtx.swapchainExtent;
    overlayRenderPassInfo.clearValueCount = 1;
    overlayRenderPassInfo.pClearValues = &clear;

    vkCmdBeginRenderPass(commandBuffer, &overlayRenderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    drawCels(commandBuffer, renderData);
    drawOverlayRects(commandBuffer, renderData);
    vkCmdEndRenderPass(commandBuffer);

    throwIfFailed(vkEndCommandBuffer(commandBuffer), "Failed to end a Vulkan command buffer.");
}
} // namespace

void initializeVulkanBackend(HWND hwnd)
{
    if (vkCtx.initialized)
        return;

    vkCtx.hwnd = hwnd;
    createInstance();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createCommandPool();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createFramebuffers();
    createUiPipeline();
    createCanvasPipeline();
    allocateCommandBuffers();
    createSyncObjects();
    createPaintPipeline();

    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    vkCtx.pendingWidth = static_cast<std::uint32_t>(std::max(1L, clientRect.right - clientRect.left));
    vkCtx.pendingHeight = static_cast<std::uint32_t>(std::max(1L, clientRect.bottom - clientRect.top));
    vkCtx.initialized = true;
}

void resizeVulkanBackend(std::uint32_t width, std::uint32_t height)
{
    if (!vkCtx.initialized || width == 0 || height == 0)
        return;

    vkCtx.pendingWidth = width;
    vkCtx.pendingHeight = height;
    vkCtx.framebufferResized = width != vkCtx.swapchainExtent.width || height != vkCtx.swapchainExtent.height;
}

static void uploadDecodedImageToTexture(const DecodedImage &image, VkImage &outImage, VkDeviceMemory &outMemory,
                                        std::uint32_t &outWidth, std::uint32_t &outHeight)
{
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(image.width) * static_cast<VkDeviceSize>(image.height) * 4;
    HostBuffer staging;
    try
    {
        createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging.buffer,
                     staging.memory);
        throwIfFailed(vkMapMemory(vkCtx.device, staging.memory, 0, imageSize, 0, &staging.mapped),
                      "Failed to map Vulkan image upload memory.");
        std::memcpy(staging.mapped, image.pixels.data(), static_cast<std::size_t>(imageSize));
        createImage(image.width, image.height, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        (vkCtx.computePainting ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                    outImage, outMemory);
        copyBufferToImage(staging.buffer, outImage, image.width, image.height);
    }
    catch (...)
    {
        destroyHostBuffer(staging);
        throw;
    }
    destroyHostBuffer(staging);

    outWidth = image.width;
    outHeight = image.height;
}

void selectCelBackend(std::uint32_t index)
{
    if (index >= g_celTextures.size())
        return;
    g_activeCel = index;
}
void setVulkanCelsBackend(const CelStack &stack)
{
    if (!vkCtx.initialized || !validCelStack(stack))
        return;
    throwIfFailed(vkDeviceWaitIdle(vkCtx.device), "Cannot synchronize cel replacement.");
    g_pendingUploads.clear();
    destroyCanvasTexture();
    g_celTextures.resize(stack.cels.size());
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::uint32_t(stack.cels.size())},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, std::uint32_t(stack.cels.size())}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = std::uint32_t(stack.cels.size()) * (vkCtx.computePainting ? 2 : 1);
    pool.poolSizeCount = vkCtx.computePainting ? 2 : 1;
    pool.pPoolSizes = poolSizes;
    throwIfFailed(vkCreateDescriptorPool(vkCtx.device, &pool, nullptr, &vkCtx.paintPool),
                  "Cannot allocate cel descriptors.");
    for (std::size_t i = 0; i < stack.cels.size(); ++i)
    {
        auto &target = g_celTextures[i];
        target.visible = stack.cels[i].visible;
        uploadDecodedImageToTexture(stack.cels[i].image, target.image, target.memory, target.width, target.height);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = target.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_B8G8R8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        throwIfFailed(vkCreateImageView(vkCtx.device, &view, nullptr, &target.view), "Cannot create cel view.");
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set.descriptorPool = vkCtx.paintPool;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &vkCtx.canvasSetLayout;
        throwIfFailed(vkAllocateDescriptorSets(vkCtx.device, &set, &target.sampled),
                      "Cannot allocate cel sampling set.");
        VkDescriptorImageInfo sampled{vkCtx.canvasSampler, target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = target.sampled;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(vkCtx.device, 1, &write, 0, nullptr);
        if (vkCtx.computePainting)
        {
            set.pSetLayouts = &vkCtx.paintSetLayout;
            throwIfFailed(vkAllocateDescriptorSets(vkCtx.device, &set, &target.paint),
                          "Cannot allocate cel painting set.");
            VkDescriptorImageInfo storage{VK_NULL_HANDLE, target.view, VK_IMAGE_LAYOUT_GENERAL};
            write.dstSet = target.paint;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &storage;
            vkUpdateDescriptorSets(vkCtx.device, 1, &write, 0, nullptr);
        }
    }
    selectCelBackend(stack.activeCel);
}

void updateVulkanCanvasRegionBackend(PendingUpload update)
{
    if (g_activeCel >= g_celTextures.size())
        return;
    const auto &cel = g_celTextures[g_activeCel];
    if (update.x >= cel.width || update.y >= cel.height || update.width > cel.width - update.x ||
        update.height > cel.height - update.y)
        return;
    update.cel = g_activeCel;
    g_pendingUploads.push_back(std::move(update));
}

void clearVulkanCanvasImageBackend()
{
    if (!vkCtx.instance)
        return;

    if (vkCtx.device)
        vkDeviceWaitIdle(vkCtx.device);
    g_pendingUploads.clear();
    destroyCanvasTexture();
}

void renderFrameVkBackend(const CanvasRenderData &renderData)
{
    if (!vkCtx.initialized || !vkCtx.hwnd)
        return;

    if (vkCtx.framebufferResized)
    {
        recreateSwapchain(vkCtx.pendingWidth, vkCtx.pendingHeight);
        if (!vkCtx.swapchain)
            return;
    }

    if (renderData.surfaceWidth != vkCtx.swapchainExtent.width ||
        renderData.surfaceHeight != vkCtx.swapchainExtent.height)
    {
        PostMessageW(vkCtx.hwnd, WM_SIMP_RENDER_READY, 0, 0);
        return;
    }

    throwIfFailed(vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFences[vkCtx.currentFrame], VK_TRUE, UINT64_MAX),
                  "Failed to wait for the Vulkan frame fence.");

    std::uint32_t imageIndex = 0;
    VkResult acquireResult =
        vkAcquireNextImageKHR(vkCtx.device, vkCtx.swapchain, UINT64_MAX,
                              vkCtx.imageAvailableSemaphores[vkCtx.currentFrame], VK_NULL_HANDLE, &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreateSwapchain(vkCtx.pendingWidth, vkCtx.pendingHeight);
        PostMessageW(vkCtx.hwnd, WM_SIMP_RENDER_READY, 0, 0);
        return;
    }

    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Failed to acquire a Vulkan swapchain image.");

    throwIfFailed(vkResetFences(vkCtx.device, 1, &vkCtx.inFlightFences[vkCtx.currentFrame]),
                  "Failed to reset the Vulkan frame fence.");
    throwIfFailed(vkResetCommandBuffer(vkCtx.commandBuffers[vkCtx.currentFrame], 0),
                  "Failed to reset a Vulkan command buffer.");
    recordCommandBuffer(vkCtx.commandBuffers[vkCtx.currentFrame], imageIndex, renderData);

    VkSemaphore waitSemaphores[] = {vkCtx.imageAvailableSemaphores[vkCtx.currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {vkCtx.renderFinishedSemaphores[imageIndex]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkCtx.commandBuffers[vkCtx.currentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    throwIfFailed(vkQueueSubmit(vkCtx.graphicsQueue, 1, &submitInfo, vkCtx.inFlightFences[vkCtx.currentFrame]),
                  "Failed to submit Vulkan work to the graphics queue.");

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vkCtx.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(vkCtx.presentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || vkCtx.framebufferResized)
    {
        recreateSwapchain(vkCtx.pendingWidth, vkCtx.pendingHeight);
    }
    else if (presentResult != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to present the Vulkan swapchain image.");
    }

    vkCtx.currentFrame = (vkCtx.currentFrame + 1) % kMaxFramesInFlight;
}

void cleanupVulkanBackend()
{
    if (!vkCtx.instance)
        return;

    if (vkCtx.device)
        vkDeviceWaitIdle(vkCtx.device);

    g_pendingUploads.clear();
    destroyCanvasTexture();

    for (VkFence fence : vkCtx.inFlightFences)
        vkDestroyFence(vkCtx.device, fence, nullptr);
    vkCtx.inFlightFences.clear();

    for (VkSemaphore semaphore : vkCtx.imageAvailableSemaphores)
        vkDestroySemaphore(vkCtx.device, semaphore, nullptr);
    vkCtx.imageAvailableSemaphores.clear();

    if (!vkCtx.commandBuffers.empty())
    {
        vkFreeCommandBuffers(vkCtx.device, vkCtx.commandPool, static_cast<std::uint32_t>(vkCtx.commandBuffers.size()),
                             vkCtx.commandBuffers.data());
        vkCtx.commandBuffers.clear();
    }

    cleanupSwapchain();
    if (vkCtx.paintPipeline)
        vkDestroyPipeline(vkCtx.device, vkCtx.paintPipeline, nullptr);
    if (vkCtx.paintLayout)
        vkDestroyPipelineLayout(vkCtx.device, vkCtx.paintLayout, nullptr);
    if (vkCtx.canvasSampler)
        vkDestroySampler(vkCtx.device, vkCtx.canvasSampler, nullptr);
    if (vkCtx.canvasLayout)
        vkDestroyPipelineLayout(vkCtx.device, vkCtx.canvasLayout, nullptr);
    if (vkCtx.canvasSetLayout)
        vkDestroyDescriptorSetLayout(vkCtx.device, vkCtx.canvasSetLayout, nullptr);
    if (vkCtx.paintSetLayout)
        vkDestroyDescriptorSetLayout(vkCtx.device, vkCtx.paintSetLayout, nullptr);
    for (auto &frame : g_frames)
    {
        destroyHostBuffer(frame.ui);
        destroyHostBuffer(frame.upload);
    }
    if (vkCtx.uiPipelineLayout)
        vkDestroyPipelineLayout(vkCtx.device, vkCtx.uiPipelineLayout, nullptr);

    if (vkCtx.commandPool)
    {
        vkDestroyCommandPool(vkCtx.device, vkCtx.commandPool, nullptr);
        vkCtx.commandPool = VK_NULL_HANDLE;
    }

    if (vkCtx.device)
    {
        vkDestroyDevice(vkCtx.device, nullptr);
        vkCtx.device = VK_NULL_HANDLE;
    }

    if (vkCtx.surface)
    {
        vkDestroySurfaceKHR(vkCtx.instance, vkCtx.surface, nullptr);
        vkCtx.surface = VK_NULL_HANDLE;
    }

    if (vkCtx.instance)
    {
        vkDestroyInstance(vkCtx.instance, nullptr);
        vkCtx.instance = VK_NULL_HANDLE;
    }

    vkCtx = {};
}

namespace
{
struct RenderCommand
{
    enum Kind
    {
        Replace,
        Update,
        SelectCel,
        Visibility,
        Clear
    } kind = Clear;
    std::optional<CelStack> stack;
    PendingUpload update;
    std::uint32_t index = 0;
    bool visible = true;
    std::uint64_t generation = 0;
};
struct RenderThread
{
    std::mutex mutex;
    std::condition_variable ready;
    std::thread thread;
    std::deque<RenderCommand> commands;
    CanvasRenderData nextFrame;
    std::exception_ptr error;
    HWND hwnd = nullptr;
    bool running = false, stop = false, frame = false, resized = false;
    std::uint32_t width = 0, height = 0;
    std::uint64_t generation = 0;
} g_renderer;

void pumpUntilSignaled(HANDLE event)
{
    bool quit = false;
    while (WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
    {
        const auto result = MsgWaitForMultipleObjectsEx(1, &event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0)
            break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                quit = true;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (quit)
        PostQuitMessage(0);
}
bool enqueueRenderCommand(RenderCommand command)
{
    {
        std::lock_guard lock(g_renderer.mutex);
        if (!g_renderer.running || g_renderer.stop)
            return false;
        if (command.kind == RenderCommand::Replace || command.kind == RenderCommand::Clear)
            ++g_renderer.generation;
        command.generation = g_renderer.generation;
        g_renderer.commands.push_back(std::move(command));
    }
    g_renderer.ready.notify_one();
    return true;
}
} // namespace
const VkPhysicalDeviceProperties &vulkanDeviceProperties()
{
    return g_publishedProperties;
}
void checkVulkanError()
{
    std::exception_ptr error;
    {
        std::lock_guard lock(g_renderer.mutex);
        error = std::exchange(g_renderer.error, {});
    }
    if (error)
        std::rethrow_exception(error);
}
void initializeVulkan(HWND hwnd)
{
    if (g_renderer.thread.joinable())
        return;
    auto initialized = std::make_shared<std::promise<void>>();
    auto future = initialized->get_future();
    HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!signal)
        throw std::runtime_error("Cannot create Vulkan initialization event.");
    g_renderer.hwnd = hwnd;
    g_renderer.stop = false;
    g_renderer.running = true;
    g_renderer.thread = std::thread([hwnd, initialized, signal] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        SetThreadDescription(GetCurrentThread(), L"SIMP Vulkan submission");
        bool started = false;
        try
        {
            initializeVulkanBackend(hwnd);
            g_publishedProperties = vkCtx.deviceProperties;
            g_publishedCompute = vkCtx.computePainting;
            initialized->set_value();
            started = true;
            SetEvent(signal);
            CanvasRenderData frame;
            std::uint64_t generation = 0;
            for (;;)
            {
                std::deque<RenderCommand> commands;
                bool draw = false, resize = false;
                std::uint32_t w = 0, h = 0;
                {
                    std::unique_lock lock(g_renderer.mutex);
                    g_renderer.ready.wait(lock, [] {
                        return g_renderer.stop || g_renderer.frame || g_renderer.resized ||
                               !g_renderer.commands.empty();
                    });
                    if (g_renderer.stop)
                        break;
                    commands.swap(g_renderer.commands);
                    draw = g_renderer.frame;
                    g_renderer.frame = false;
                    if (draw)
                        std::swap(frame, g_renderer.nextFrame);
                    resize = g_renderer.resized;
                    g_renderer.resized = false;
                    w = g_renderer.width;
                    h = g_renderer.height;
                }
                if (resize)
                    resizeVulkanBackend(w, h);
                for (auto &command : commands)
                {
                    if (command.kind == RenderCommand::Replace)
                    {
                        setVulkanCelsBackend(*command.stack);
                        generation = command.generation;
                    }
                    else if (command.kind == RenderCommand::Clear)
                    {
                        clearVulkanCanvasImageBackend();
                        generation = command.generation;
                    }
                    else if (command.generation != generation)
                        continue;
                    else if (command.kind == RenderCommand::SelectCel)
                        selectCelBackend(command.index);
                    else if (command.kind == RenderCommand::Visibility)
                    {
                        if (command.index < g_celTextures.size())
                            g_celTextures[command.index].visible = command.visible;
                    }
                    else if (command.kind == RenderCommand::Update)
                        updateVulkanCanvasRegionBackend(std::move(command.update));
                }
                if (draw && !IsIconic(hwnd))
                {
                    if (frame.canvasGeneration == generation)
                        renderFrameVkBackend(frame);
                    else
                        PostMessageW(hwnd, WM_SIMP_RENDER_READY, 0, 0);
                }
            }
        }
        catch (...)
        {
            if (!started)
            {
                initialized->set_exception(std::current_exception());
                SetEvent(signal);
            }
            else
            {
                std::lock_guard lock(g_renderer.mutex);
                g_renderer.error = std::current_exception();
                PostMessageW(hwnd, WM_SIMP_RENDER_READY, 0, 0);
            }
        }
        cleanupVulkanBackend();
        std::lock_guard lock(g_renderer.mutex);
        g_renderer.running = false;
    });
    pumpUntilSignaled(signal);
    CloseHandle(signal);
    future.get();
}
void resizeVulkan(std::uint32_t width, std::uint32_t height)
{
    {
        std::lock_guard lock(g_renderer.mutex);
        if (!g_renderer.running || !width || !height)
            return;
        g_renderer.width = width;
        g_renderer.height = height;
        g_renderer.resized = true;
    }
    g_renderer.ready.notify_one();
}
void setVulkanCels(const CelStack &stack)
{
    RenderCommand command;
    command.kind = RenderCommand::Replace;
    command.stack = stack;
    enqueueRenderCommand(std::move(command));
}
void selectVulkanCel(std::uint32_t index)
{
    RenderCommand command;
    command.kind = RenderCommand::SelectCel;
    command.index = index;
    enqueueRenderCommand(std::move(command));
}
void setVulkanCelVisibility(std::uint32_t index, bool visible)
{
    RenderCommand command;
    command.kind = RenderCommand::Visibility;
    command.index = index;
    command.visible = visible;
    enqueueRenderCommand(std::move(command));
}
void updateVulkanCanvasRegion(const DecodedImage &image, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                              std::uint32_t height)
{
    if (!width || !height || x >= image.width || y >= image.height || width > image.width - x ||
        height > image.height - y)
        return;
    RenderCommand command;
    command.kind = RenderCommand::Update;
    auto &update = command.update;
    update.x = x;
    update.y = y;
    update.width = width;
    update.height = height;
    update.pixels.resize(std::size_t(width) * height * 4);
    for (std::uint32_t row = 0; row < height; ++row)
        std::memcpy(update.pixels.data() + std::size_t(row) * width * 4,
                    image.pixels.data() + (std::size_t(y + row) * image.width + x) * 4, std::size_t(width) * 4);
    enqueueRenderCommand(std::move(command));
}
bool paintVulkanCanvasRegion(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                             std::uint32_t bgra)
{
    if (!g_publishedCompute)
        return false;
    RenderCommand command;
    command.kind = RenderCommand::Update;
    command.update.paint = true;
    command.update.x = x;
    command.update.y = y;
    command.update.width = width;
    command.update.height = height;
    command.update.color = bgra;
    return enqueueRenderCommand(std::move(command));
}
void clearVulkanCanvasImage()
{
    enqueueRenderCommand({});
}
void renderFrameVk()
{
    RECT client{};
    if (!GetClientRect(g_renderer.hwnd, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    auto &frame = buildCanvasRenderData(client.right, client.bottom);
    {
        std::lock_guard lock(g_renderer.mutex);
        if (!g_renderer.running || g_renderer.stop)
            return;
        std::swap(g_renderer.nextFrame, frame);
        g_renderer.nextFrame.canvasGeneration = g_renderer.generation;
        g_renderer.frame = true;
    }
    g_renderer.ready.notify_one();
}
void cleanupVulkan()
{
    if (!g_renderer.thread.joinable())
        return;
    {
        std::lock_guard lock(g_renderer.mutex);
        g_renderer.stop = true;
    }
    g_renderer.ready.notify_all();
    pumpUntilSignaled(g_renderer.thread.native_handle());
    g_renderer.thread.join();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.commands.clear();
    g_renderer.nextFrame = {};
}
