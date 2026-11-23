// Headless shader correctness test. The application still rejects software GPUs.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "vulkan_test_device.h"
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    TestDevice test;
    const VkApplicationInfo app{
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "SIMP compute test", 1, nullptr, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &app;
    check(vkCreateInstance(&instance, nullptr, &test.instance));
    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(test.instance, &count, nullptr));
    if (!count)
        return 77;
    std::vector<VkPhysicalDevice> physicals(count);
    check(vkEnumeratePhysicalDevices(test.instance, &count, physicals.data()));
    const auto physical = physicals.front();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()));
    const char *extension = VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME;
    if (!features.shaderStorageImageWriteWithoutFormat ||
        std::none_of(extensions.begin(), extensions.end(),
                     [&](const auto &e) { return !std::strcmp(e.extensionName, extension); }))
    {
        std::cout << "Compute painting unavailable on " << properties.deviceName << ".\n";
        return 77;
    }
    VkFormatProperties3 format3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 format2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    format2.pNext = &format3;
    vkGetPhysicalDeviceFormatProperties2(physical, VK_FORMAT_B8G8R8A8_UNORM, &format2);
    const auto required = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if ((format3.optimalTilingFeatures & required) != required)
    {
        std::cout << "BGRA compute painting unavailable on " << properties.deviceName << ".\n";
        return 77;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++family;
    if (family == count)
        return 77;
    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageWriteWithoutFormat = true;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queueInfo;
    device.enabledExtensionCount = 1;
    device.ppEnabledExtensionNames = &extension;
    device.pEnabledFeatures = &enabled;
    check(vkCreateDevice(physical, &device, nullptr, &test.device));
    VkQueue queue{};
    vkGetDeviceQueue(test.device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const auto allocate = [&](VkMemoryRequirements requirements, VkMemoryPropertyFlags flags, VkDeviceMemory &out) {
        std::uint32_t type = 0;
        while (type < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
                                                 (memory.memoryTypes[type].propertyFlags & flags) != flags))
            ++type;
        if (type == memory.memoryTypeCount)
            throw std::runtime_error("No test memory type.");
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = type;
        check(vkAllocateMemory(test.device, &info, nullptr, &out));
    };
    constexpr std::uint32_t width = 263, height = 19, bytes = width * height * 4;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_B8G8R8A8_UNORM;
    image.extent = {width, height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateImage(test.device, &image, nullptr, &test.image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(test.device, test.image, &requirements);
    allocate(requirements, 0, test.imageMemory);
    check(vkBindImageMemory(test.device, test.image, test.imageMemory, 0));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = test.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(test.device, &view, nullptr, &test.view));
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(test.device, &buffer, nullptr, &test.buffer));
    vkGetBufferMemoryRequirements(test.device, test.buffer, &requirements);
    allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             test.bufferMemory);
    check(vkBindBufferMemory(test.device, test.buffer, test.bufferMemory, 0));
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = 1;
    set.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(test.device, &set, nullptr, &test.setLayout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &test.setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(test.device, &layout, nullptr, &test.layout));
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    check(vkCreateDescriptorPool(test.device, &pool, nullptr, &test.descriptors));
    VkDescriptorSet descriptor{};
    VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sets.descriptorPool = test.descriptors;
    sets.descriptorSetCount = 1;
    sets.pSetLayouts = &test.setLayout;
    check(vkAllocateDescriptorSets(test.device, &sets, &descriptor));
    const VkDescriptorImageInfo target{VK_NULL_HANDLE, test.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &target;
    vkUpdateDescriptorSets(test.device, 1, &write, 0, nullptr);
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() % 4)
        throw std::runtime_error("Invalid compute module.");
    std::vector<std::uint32_t> code(std::size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), code.size() * 4);
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = code.size() * 4;
    shader.pCode = code.data();
    check(vkCreateShaderModule(test.device, &shader, nullptr, &test.shader));
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = test.layout;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = test.shader;
    pipeline.stage.pName = "main";
    check(vkCreateComputePipelines(test.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &test.pipeline));
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commands.queueFamilyIndex = family;
    check(vkCreateCommandPool(test.device, &commands, nullptr, &test.commands));
    VkCommandBuffer command{};
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = test.commands;
    commandInfo.commandBufferCount = 1;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    check(vkAllocateCommandBuffers(test.device, &commandInfo, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin));
    const auto barrier = [&](VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags from,
                             VkPipelineStageFlags to, VkAccessFlags reads, VkAccessFlags writes) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcAccessMask = reads;
        b.dstAccessMask = writes;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = test.image;
        b.subresourceRange = view.subresourceRange;
        vkCmdPipelineBarrier(command, from, to, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, test.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, test.layout, 0, 1, &descriptor, 0, nullptr);
    std::vector<std::uint32_t> expected(width * height);
    const auto paint = [&](int x, int y, int w, int h, std::uint32_t color) {
        const struct
        {
            std::int32_t rect[4];
            float color[4];
        } values{{x, y, w, h},
                 {float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255, float(color & 255) / 255,
                  float(color >> 24) / 255}};
        vkCmdPushConstants(command, test.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(values), &values);
        vkCmdDispatch(command, (w + 15) / 16, (h + 7) / 8, 1);
        // Same conservative dependency used between editor operations.
        barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        for (int row = y; row < y + h; ++row)
            std::fill_n(expected.begin() + row * width + x, w, color);
    };
    paint(0, 0, width, height, 0x3f726591);
    for (std::uint32_t c = 0; c < 256; ++c)
        paint(c, 1, 1, 17, (c << 24) | ((255 - c) << 16) | (((c * 73) & 255) << 8) | c);
    paint(11, 4, 67, 9, 0xa1276ec9);
    paint(250, 16, 13, 3, 0x017f80fe);
    barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command, test.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, test.buffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                         0, nullptr);
    check(vkEndCommandBuffer(command));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    check(vkQueueWaitIdle(queue));
    void *mapped = nullptr;
    check(vkMapMemory(test.device, test.bufferMemory, 0, bytes, 0, &mapped));
    const bool equal = !std::memcmp(mapped, expected.data(), bytes);
    vkUnmapMemory(test.device, test.bufferMemory);
    if (!equal)
        throw std::runtime_error("Compute paint differs from lossless CPU reference.");
    std::cout << "Compute BGRA pixels, alpha, overlapping edits and dispatch edges match CPU on "
              << properties.deviceName << ".\n";
}
