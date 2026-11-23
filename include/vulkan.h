#ifndef SIMP_VULKAN_H
#define SIMP_VULKAN_H

#define VK_USE_PLATFORM_WIN32_KHR

#include <cstdint>
#include <vector>

#include <windows.h>

#include <vulkan/vulkan.h>

struct DecodedImage;
struct CelStack;

// Only the Vulkan submission thread owns the mutable context.
const VkPhysicalDeviceProperties &vulkanDeviceProperties();
void checkVulkanError();
constexpr UINT WM_SIMP_RENDER_READY = WM_APP + 41;

void initializeVulkan(HWND hwnd);
void resizeVulkan(std::uint32_t width, std::uint32_t height);
void setVulkanCels(const CelStack &);
void selectVulkanCel(std::uint32_t);
void setVulkanCelVisibility(std::uint32_t, bool);
void updateVulkanCanvasRegion(const DecodedImage &image, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                              std::uint32_t height);
bool paintVulkanCanvasRegion(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                             std::uint32_t bgra);
void clearVulkanCanvasImage();
void renderFrameVk();
void cleanupVulkan();

#endif
