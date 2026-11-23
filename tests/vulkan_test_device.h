#pragma once
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>
inline void check(VkResult result)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan test failed: " + std::to_string(result));
}
struct TestDevice
{
    VkInstance instance{};
    VkDevice device{};
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory imageMemory{}, bufferMemory{};
    VkBuffer buffer{};
    VkDescriptorPool descriptors{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkShaderModule shader{};
    VkCommandPool commands{};
    ~TestDevice()
    {
        if (device)
        {
            vkDeviceWaitIdle(device);
            vkDestroyCommandPool(device, commands, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyShaderModule(device, shader, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyDescriptorPool(device, descriptors, nullptr);
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            vkDestroyImageView(device, view, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, imageMemory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, bufferMemory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
    }
};
