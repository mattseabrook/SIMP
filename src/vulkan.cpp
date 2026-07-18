#include "vulkan.h"

#include "app.h"
#include "system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

#include <vulkan/vulkan_win32.h>

VulkanContext vkCtx;

namespace
{
constexpr std::size_t kMaxFramesInFlight = 2;
constexpr std::array<const char*, 1> kRequiredDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

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

void throwIfFailed(VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(message);
}

std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    const VkPhysicalDeviceMemoryProperties& memoryProperties = vkCtx.memoryProperties;

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

void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory)
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

    throwIfFailed(vkAllocateMemory(vkCtx.device, &allocateInfo, nullptr, &memory), "Failed to allocate Vulkan buffer memory.");
    throwIfFailed(vkBindBufferMemory(vkCtx.device, buffer, memory, 0), "Failed to bind Vulkan buffer memory.");
}

void createImage(
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory)
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

    throwIfFailed(vkAllocateMemory(vkCtx.device, &allocateInfo, nullptr, &memory), "Failed to allocate Vulkan image memory.");
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
    throwIfFailed(vkAllocateCommandBuffers(vkCtx.device, &allocateInfo, &commandBuffer), "Failed to allocate a Vulkan upload command buffer.");

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

    throwIfFailed(vkQueueSubmit(vkCtx.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Failed to submit Vulkan upload work.");
    throwIfFailed(vkQueueWaitIdle(vkCtx.graphicsQueue), "Failed to wait for Vulkan upload work.");
    vkFreeCommandBuffers(vkCtx.device, vkCtx.commandPool, 1, &commandBuffer);
}

void transitionImageLayout(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage,
    VkAccessFlags sourceAccess,
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

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

void copyBufferToImage(VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height)
{
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    transitionImageLayout(
        commandBuffer,
        image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionImageLayout(
        commandBuffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);

    endSingleTimeCommands(commandBuffer);
}

void destroyGpuImage(VkImage& image, VkDeviceMemory& memory, std::uint32_t& width, std::uint32_t& height)
{
    if (image)
    {
        vkDestroyImage(vkCtx.device, image, nullptr);
        image = VK_NULL_HANDLE;
    }

    if (memory)
    {
        vkFreeMemory(vkCtx.device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }

    width = 0;
    height = 0;
}

void destroyCanvasTexture()
{
    destroyGpuImage(vkCtx.canvasImage, vkCtx.canvasImageMemory, vkCtx.canvasImageWidth, vkCtx.canvasImageHeight);
}

void destroyToolbarTexture()
{
    destroyGpuImage(vkCtx.toolbarImage, vkCtx.toolbarImageMemory, vkCtx.toolbarImageWidth, vkCtx.toolbarImageHeight);
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

        if (indices.complete())
            break;
    }

    return indices;
}

bool checkDeviceExtensionSupport(VkPhysicalDevice device)
{
    std::uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    for (const char* requiredExtension : kRequiredDeviceExtensions)
    {
        bool found = false;
        for (const VkExtensionProperties& extension : availableExtensions)
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
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, vkCtx.surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
{
    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }

    return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes)
{
    for (const VkPresentModeKHR mode : presentModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities)
{
    if (capabilities.currentExtent.width != UINT32_MAX)
        return capabilities.currentExtent;

    RECT clientRect{};
    GetClientRect(vkCtx.hwnd, &clientRect);

    VkExtent2D extent{};
    extent.width = std::clamp(
        static_cast<std::uint32_t>(std::max(1L, clientRect.right - clientRect.left)),
        capabilities.minImageExtent.width,
        capabilities.maxImageExtent.width);
    extent.height = std::clamp(
        static_cast<std::uint32_t>(std::max(1L, clientRect.bottom - clientRect.top)),
        capabilities.minImageExtent.height,
        capabilities.maxImageExtent.height);
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
    applicationInfo.pApplicationName = "SIMP";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.pEngineName = "SIMP";
    applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_1;

    constexpr std::array<const char*, 2> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

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

    throwIfFailed(vkCreateWin32SurfaceKHR(vkCtx.instance, &createInfo, nullptr, &vkCtx.surface), "Failed to create the Vulkan Win32 surface.");
}

void pickPhysicalDevice()
{
    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vkCtx.instance, &deviceCount, nullptr);
    if (deviceCount == 0)
        throw std::runtime_error("No Vulkan-compatible GPU was found.");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkCtx.instance, &deviceCount, devices.data());

    for (VkPhysicalDevice device : devices)
    {
        if (isPhysicalDeviceSuitable(device))
        {
            vkCtx.physicalDevice = device;
            vkGetPhysicalDeviceMemoryProperties(device, &vkCtx.memoryProperties);
            return;
        }
    }

    throw std::runtime_error("Failed to find a Vulkan GPU that supports presentation.");
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

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(kRequiredDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions.data();

    throwIfFailed(vkCreateDevice(vkCtx.physicalDevice, &createInfo, nullptr, &vkCtx.device), "Failed to create the Vulkan logical device.");

    vkGetDeviceQueue(vkCtx.device, vkCtx.graphicsQueueFamily, 0, &vkCtx.graphicsQueue);
    vkGetDeviceQueue(vkCtx.device, vkCtx.presentQueueFamily, 0, &vkCtx.presentQueue);
}

void createCommandPool()
{
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = vkCtx.graphicsQueueFamily;

    throwIfFailed(vkCreateCommandPool(vkCtx.device, &createInfo, nullptr, &vkCtx.commandPool), "Failed to create the Vulkan command pool.");
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
    if ((support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
        throw std::runtime_error("The Vulkan swapchain does not support image blits.");

    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

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
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    throwIfFailed(vkCreateSwapchainKHR(vkCtx.device, &createInfo, nullptr, &vkCtx.swapchain), "Failed to create the Vulkan swapchain.");

    vkGetSwapchainImagesKHR(vkCtx.device, vkCtx.swapchain, &imageCount, nullptr);
    vkCtx.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(vkCtx.device, vkCtx.swapchain, &imageCount, vkCtx.swapchainImages.data());
    vkCtx.imagesInFlight.assign(imageCount, VK_NULL_HANDLE);

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

        throwIfFailed(vkCreateImageView(vkCtx.device, &createInfo, nullptr, &vkCtx.swapchainImageViews[index]), "Failed to create a Vulkan swapchain image view.");
    }
}

void createRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = vkCtx.swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    throwIfFailed(vkCreateRenderPass(vkCtx.device, &renderPassInfo, nullptr, &vkCtx.renderPass), "Failed to create the Vulkan render pass.");

    VkAttachmentDescription overlayAttachment = colorAttachment;
    overlayAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    overlayAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlayAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkRenderPassCreateInfo overlayRenderPassInfo{};
    overlayRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    overlayRenderPassInfo.attachmentCount = 1;
    overlayRenderPassInfo.pAttachments = &overlayAttachment;
    overlayRenderPassInfo.subpassCount = 1;
    overlayRenderPassInfo.pSubpasses = &subpass;
    overlayRenderPassInfo.dependencyCount = 1;
    overlayRenderPassInfo.pDependencies = &dependency;

    throwIfFailed(vkCreateRenderPass(vkCtx.device, &overlayRenderPassInfo, nullptr, &vkCtx.overlayRenderPass), "Failed to create the Vulkan overlay render pass.");
}

void createFramebuffers()
{
    vkCtx.framebuffers.resize(vkCtx.swapchainImageViews.size());
    vkCtx.overlayFramebuffers.resize(vkCtx.swapchainImageViews.size());

    for (std::size_t index = 0; index < vkCtx.swapchainImageViews.size(); ++index)
    {
        VkImageView attachments[] = {vkCtx.swapchainImageViews[index]};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = vkCtx.renderPass;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = attachments;
        createInfo.width = vkCtx.swapchainExtent.width;
        createInfo.height = vkCtx.swapchainExtent.height;
        createInfo.layers = 1;

        throwIfFailed(vkCreateFramebuffer(vkCtx.device, &createInfo, nullptr, &vkCtx.framebuffers[index]), "Failed to create a Vulkan framebuffer.");

        createInfo.renderPass = vkCtx.overlayRenderPass;
        throwIfFailed(vkCreateFramebuffer(vkCtx.device, &createInfo, nullptr, &vkCtx.overlayFramebuffers[index]), "Failed to create a Vulkan overlay framebuffer.");
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

    throwIfFailed(vkAllocateCommandBuffers(vkCtx.device, &allocateInfo, vkCtx.commandBuffers.data()), "Failed to allocate Vulkan command buffers.");
}

void createSyncObjects()
{
    vkCtx.imageAvailableSemaphores.resize(kMaxFramesInFlight);
    vkCtx.renderFinishedSemaphores.resize(kMaxFramesInFlight);
    vkCtx.inFlightFences.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (std::size_t index = 0; index < kMaxFramesInFlight; ++index)
    {
        throwIfFailed(vkCreateSemaphore(vkCtx.device, &semaphoreInfo, nullptr, &vkCtx.imageAvailableSemaphores[index]), "Failed to create a Vulkan semaphore.");
        throwIfFailed(vkCreateSemaphore(vkCtx.device, &semaphoreInfo, nullptr, &vkCtx.renderFinishedSemaphores[index]), "Failed to create a Vulkan semaphore.");
        throwIfFailed(vkCreateFence(vkCtx.device, &fenceInfo, nullptr, &vkCtx.inFlightFences[index]), "Failed to create a Vulkan fence.");
    }
}

void cleanupSwapchain()
{
    for (VkFramebuffer framebuffer : vkCtx.overlayFramebuffers)
        vkDestroyFramebuffer(vkCtx.device, framebuffer, nullptr);
    vkCtx.overlayFramebuffers.clear();

    for (VkFramebuffer framebuffer : vkCtx.framebuffers)
        vkDestroyFramebuffer(vkCtx.device, framebuffer, nullptr);
    vkCtx.framebuffers.clear();

    if (vkCtx.overlayRenderPass)
    {
        vkDestroyRenderPass(vkCtx.device, vkCtx.overlayRenderPass, nullptr);
        vkCtx.overlayRenderPass = VK_NULL_HANDLE;
    }

    if (vkCtx.renderPass)
    {
        vkDestroyRenderPass(vkCtx.device, vkCtx.renderPass, nullptr);
        vkCtx.renderPass = VK_NULL_HANDLE;
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
    vkCtx.imagesInFlight.clear();
}

void recreateSwapchain(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    vkDeviceWaitIdle(vkCtx.device);
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createFramebuffers();
    vkCtx.framebufferResized = false;
}

void blitCanvasImageToSwapchain(VkCommandBuffer commandBuffer, VkImage swapchainImage, const CanvasRenderData& renderData)
{
    if (!renderData.hasImage || !vkCtx.canvasImage || vkCtx.canvasImageWidth == 0 || vkCtx.canvasImageHeight == 0)
        return;

    const float imageLeft = renderData.imageX;
    const float imageTop = renderData.imageY;
    const float imageRight = renderData.imageX + renderData.imageWidth;
    const float imageBottom = renderData.imageY + renderData.imageHeight;
    const float clipLeft = std::max(0.0f, imageLeft);
    const float clipTop = std::max(0.0f, imageTop);
    const float clipRight = std::min(static_cast<float>(vkCtx.swapchainExtent.width), imageRight);
    const float clipBottom = std::min(static_cast<float>(vkCtx.swapchainExtent.height), imageBottom);

    if (clipRight <= clipLeft || clipBottom <= clipTop || renderData.imageWidth <= 0.0f || renderData.imageHeight <= 0.0f)
        return;

    const float u0 = (clipLeft - imageLeft) / renderData.imageWidth;
    const float v0 = (clipTop - imageTop) / renderData.imageHeight;
    const float u1 = (clipRight - imageLeft) / renderData.imageWidth;
    const float v1 = (clipBottom - imageTop) / renderData.imageHeight;

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = 0;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = {
        static_cast<std::int32_t>(std::floor(u0 * static_cast<float>(vkCtx.canvasImageWidth))),
        static_cast<std::int32_t>(std::floor(v0 * static_cast<float>(vkCtx.canvasImageHeight))),
        0};
    blit.srcOffsets[1] = {
        static_cast<std::int32_t>(std::ceil(u1 * static_cast<float>(vkCtx.canvasImageWidth))),
        static_cast<std::int32_t>(std::ceil(v1 * static_cast<float>(vkCtx.canvasImageHeight))),
        1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = 0;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {
        static_cast<std::int32_t>(std::floor(clipLeft)),
        static_cast<std::int32_t>(std::floor(clipTop)),
        0};
    blit.dstOffsets[1] = {
        static_cast<std::int32_t>(std::ceil(clipRight)),
        static_cast<std::int32_t>(std::ceil(clipBottom)),
        1};

    transitionImageLayout(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    vkCmdBlitImage(
        commandBuffer,
        vkCtx.canvasImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blit,
        VK_FILTER_NEAREST);

    transitionImageLayout(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

void blitToolbarImageToSwapchain(VkCommandBuffer commandBuffer, VkImage swapchainImage)
{
    if (!vkCtx.toolbarImage || vkCtx.toolbarImageWidth == 0 || vkCtx.toolbarImageHeight == 0)
        return;

    const std::int32_t dstWidth = std::min(static_cast<std::int32_t>(vkCtx.toolbarImageWidth), static_cast<std::int32_t>(vkCtx.swapchainExtent.width));
    const std::int32_t dstHeight = std::min(static_cast<std::int32_t>(vkCtx.toolbarImageHeight), static_cast<std::int32_t>(vkCtx.swapchainExtent.height));
    if (dstWidth <= 0 || dstHeight <= 0)
        return;

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = 0;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {dstWidth, dstHeight, 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = 0;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {dstWidth, dstHeight, 1};

    transitionImageLayout(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    vkCmdBlitImage(
        commandBuffer,
        vkCtx.toolbarImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blit,
        VK_FILTER_NEAREST);

    transitionImageLayout(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

void drawOverlayRects(VkCommandBuffer commandBuffer, const CanvasRenderData& renderData)
{
    for (const CanvasOverlayRect& rect : renderData.overlays)
    {
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color.float32[0] = rect.red;
        attachment.clearValue.color.float32[1] = rect.green;
        attachment.clearValue.color.float32[2] = rect.blue;
        attachment.clearValue.color.float32[3] = rect.alpha;

        VkClearRect clearRect{};
        clearRect.rect.offset = {rect.x, rect.y};
        clearRect.rect.extent = {rect.width, rect.height};
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;
        vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clearRect);
    }
}

void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    throwIfFailed(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin a Vulkan command buffer.");

    const CanvasRenderData renderData = buildCanvasRenderData(vkCtx.swapchainExtent.width, vkCtx.swapchainExtent.height);
    VkClearValue clearColor{};
    clearColor.color.float32[0] = 0.8f;
    clearColor.color.float32[1] = 0.8f;
    clearColor.color.float32[2] = 0.8f;
    clearColor.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = vkCtx.renderPass;
    renderPassInfo.framebuffer = vkCtx.framebuffers.at(imageIndex);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = vkCtx.swapchainExtent;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(commandBuffer);

    blitCanvasImageToSwapchain(commandBuffer, vkCtx.swapchainImages.at(imageIndex), renderData);
    blitToolbarImageToSwapchain(commandBuffer, vkCtx.swapchainImages.at(imageIndex));

    VkRenderPassBeginInfo overlayRenderPassInfo{};
    overlayRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    overlayRenderPassInfo.renderPass = vkCtx.overlayRenderPass;
    overlayRenderPassInfo.framebuffer = vkCtx.overlayFramebuffers.at(imageIndex);
    overlayRenderPassInfo.renderArea.offset = {0, 0};
    overlayRenderPassInfo.renderArea.extent = vkCtx.swapchainExtent;

    vkCmdBeginRenderPass(commandBuffer, &overlayRenderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    drawOverlayRects(commandBuffer, renderData);
    vkCmdEndRenderPass(commandBuffer);

    throwIfFailed(vkEndCommandBuffer(commandBuffer), "Failed to end a Vulkan command buffer.");
}
}

void initializeVulkan(HWND hwnd)
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
    allocateCommandBuffers();
    createSyncObjects();

    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    vkCtx.pendingWidth = static_cast<std::uint32_t>(std::max(1L, clientRect.right - clientRect.left));
    vkCtx.pendingHeight = static_cast<std::uint32_t>(std::max(1L, clientRect.bottom - clientRect.top));
    vkCtx.initialized = true;
}

void resizeVulkan(std::uint32_t width, std::uint32_t height)
{
    if (!vkCtx.initialized || width == 0 || height == 0)
        return;

    vkCtx.pendingWidth = width;
    vkCtx.pendingHeight = height;
    vkCtx.framebufferResized = true;
}

static void uploadDecodedImageToTexture(const DecodedImage& image, VkImage& outImage, VkDeviceMemory& outMemory, std::uint32_t& outWidth, std::uint32_t& outHeight)
{
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(image.width) * static_cast<VkDeviceSize>(image.height) * 4;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(
        imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory);

    void* mapped = nullptr;
    throwIfFailed(vkMapMemory(vkCtx.device, stagingMemory, 0, imageSize, 0, &mapped), "Failed to map Vulkan image upload memory.");
    std::memcpy(mapped, image.pixels.data(), static_cast<std::size_t>(imageSize));
    vkUnmapMemory(vkCtx.device, stagingMemory);

    createImage(
        image.width,
        image.height,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        outImage,
        outMemory);

    copyBufferToImage(stagingBuffer, outImage, image.width, image.height);
    vkDestroyBuffer(vkCtx.device, stagingBuffer, nullptr);
    vkFreeMemory(vkCtx.device, stagingMemory, nullptr);

    outWidth = image.width;
    outHeight = image.height;
}

void setVulkanCanvasImage(const DecodedImage& image)
{
    if (!vkCtx.initialized || image.width == 0 || image.height == 0 || image.pixels.empty())
        return;

    vkDeviceWaitIdle(vkCtx.device);
    destroyCanvasTexture();
    uploadDecodedImageToTexture(image, vkCtx.canvasImage, vkCtx.canvasImageMemory, vkCtx.canvasImageWidth, vkCtx.canvasImageHeight);
}

void clearVulkanCanvasImage()
{
    if (!vkCtx.initialized)
        return;

    vkDeviceWaitIdle(vkCtx.device);
    destroyCanvasTexture();
}

void setVulkanToolbarImage(const DecodedImage& image)
{
    if (!vkCtx.initialized || image.width == 0 || image.height == 0 || image.pixels.empty())
        return;

    vkDeviceWaitIdle(vkCtx.device);
    destroyToolbarTexture();
    uploadDecodedImageToTexture(image, vkCtx.toolbarImage, vkCtx.toolbarImageMemory, vkCtx.toolbarImageWidth, vkCtx.toolbarImageHeight);
}

void renderFrameVk()
{
    if (!vkCtx.initialized || !vkCtx.hwnd)
        return;

    if (vkCtx.framebufferResized)
    {
        recreateSwapchain(vkCtx.pendingWidth, vkCtx.pendingHeight);
        if (!vkCtx.swapchain)
            return;
    }

    throwIfFailed(
        vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFences[vkCtx.currentFrame], VK_TRUE, UINT64_MAX),
        "Failed to wait for the Vulkan frame fence.");

    std::uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(
        vkCtx.device,
        vkCtx.swapchain,
        UINT64_MAX,
        vkCtx.imageAvailableSemaphores[vkCtx.currentFrame],
        VK_NULL_HANDLE,
        &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreateSwapchain(vkCtx.pendingWidth, vkCtx.pendingHeight);
        return;
    }

    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Failed to acquire a Vulkan swapchain image.");

    if (vkCtx.imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    {
        throwIfFailed(
            vkWaitForFences(vkCtx.device, 1, &vkCtx.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX),
            "Failed to wait for a Vulkan swapchain image fence.");
    }
    vkCtx.imagesInFlight[imageIndex] = vkCtx.inFlightFences[vkCtx.currentFrame];

    throwIfFailed(vkResetFences(vkCtx.device, 1, &vkCtx.inFlightFences[vkCtx.currentFrame]), "Failed to reset the Vulkan frame fence.");
    throwIfFailed(vkResetCommandBuffer(vkCtx.commandBuffers[vkCtx.currentFrame], 0), "Failed to reset a Vulkan command buffer.");
    recordCommandBuffer(vkCtx.commandBuffers[vkCtx.currentFrame], imageIndex);

    VkSemaphore waitSemaphores[] = {vkCtx.imageAvailableSemaphores[vkCtx.currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {vkCtx.renderFinishedSemaphores[vkCtx.currentFrame]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkCtx.commandBuffers[vkCtx.currentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    throwIfFailed(vkQueueSubmit(vkCtx.graphicsQueue, 1, &submitInfo, vkCtx.inFlightFences[vkCtx.currentFrame]), "Failed to submit Vulkan work to the graphics queue.");

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

void cleanupVulkan()
{
    if (!vkCtx.initialized)
        return;

    vkDeviceWaitIdle(vkCtx.device);

    destroyCanvasTexture();
    destroyToolbarTexture();

    for (VkFence fence : vkCtx.inFlightFences)
        vkDestroyFence(vkCtx.device, fence, nullptr);
    vkCtx.inFlightFences.clear();

    for (VkSemaphore semaphore : vkCtx.renderFinishedSemaphores)
        vkDestroySemaphore(vkCtx.device, semaphore, nullptr);
    vkCtx.renderFinishedSemaphores.clear();

    for (VkSemaphore semaphore : vkCtx.imageAvailableSemaphores)
        vkDestroySemaphore(vkCtx.device, semaphore, nullptr);
    vkCtx.imageAvailableSemaphores.clear();

    if (!vkCtx.commandBuffers.empty())
    {
        vkFreeCommandBuffers(
            vkCtx.device,
            vkCtx.commandPool,
            static_cast<std::uint32_t>(vkCtx.commandBuffers.size()),
            vkCtx.commandBuffers.data());
        vkCtx.commandBuffers.clear();
    }

    cleanupSwapchain();

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
