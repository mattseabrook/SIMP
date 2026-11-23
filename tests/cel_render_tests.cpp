#include "vulkan_test_device.h"
#include "cels.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

struct CelDevice : TestDevice
{
    VkSampler sampler{};
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    VkShaderModule fragment{};
    std::array<VkImage, 2> textures{};
    std::array<VkImageView, 2> views{};
    std::array<VkDeviceMemory, 2> memories{};
    ~CelDevice()
    {
        if (!device)
            return;
        vkDeviceWaitIdle(device);
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        vkDestroyRenderPass(device, pass, nullptr);
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
        for (int i = 0; i < 2; ++i)
        {
            vkDestroyImageView(device, views[i], nullptr);
            vkDestroyImage(device, textures[i], nullptr);
            vkFreeMemory(device, memories[i], nullptr);
        }
    }
};
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    constexpr std::uint32_t w = 17, h = 13, bytes = w * h * 4;
    CelDevice test;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SIMP cel graphics test";
    app.apiVersion = VK_API_VERSION_1_1;
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
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    if (family == count)
        return 77;
    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queueInfo;
    check(vkCreateDevice(physical, &device, nullptr, &test.device));
    VkQueue queue{};
    vkGetDeviceQueue(test.device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const auto allocate = [&](VkMemoryRequirements requirement, VkMemoryPropertyFlags flags, VkDeviceMemory &output) {
        std::uint32_t type = 0;
        while (type < memory.memoryTypeCount && (!(requirement.memoryTypeBits & (1u << type)) ||
                                                 (memory.memoryTypes[type].propertyFlags & flags) != flags))
            ++type;
        if (type == memory.memoryTypeCount)
            throw std::runtime_error("No cel test memory.");
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = requirement.size;
        info.memoryTypeIndex = type;
        check(vkAllocateMemory(test.device, &info, nullptr, &output));
    };
    const auto image = [&](VkImageUsageFlags usage, VkImage &output, VkDeviceMemory &storage, VkImageView &view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_B8G8R8A8_UNORM;
        info.extent = {w, h, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        check(vkCreateImage(test.device, &info, nullptr, &output));
        VkMemoryRequirements required{};
        vkGetImageMemoryRequirements(test.device, output, &required);
        allocate(required, 0, storage);
        check(vkBindImageMemory(test.device, output, storage, 0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = output;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = info.format;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(test.device, &v, nullptr, &view));
    };
    image(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, test.image, test.imageMemory,
          test.view);
    for (int i = 0; i < 2; ++i)
        image(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, test.textures[i], test.memories[i],
              test.views[i]);
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes * 2;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateBuffer(test.device, &buffer, nullptr, &test.buffer));
    VkMemoryRequirements required{};
    vkGetBufferMemoryRequirements(test.device, test.buffer, &required);
    allocate(required, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, test.bufferMemory);
    check(vkBindBufferMemory(test.device, test.buffer, test.bufferMemory, 0));
    void *mapped = nullptr;
    check(vkMapMemory(test.device, test.bufferMemory, 0, bytes * 2, 0, &mapped));
    CelStack stack;
    stack.cels.resize(2);
    stack.cels[1].id = 2;
    for (int layer = 0; layer < 2; ++layer)
    {
        stack.cels[layer].image = {w, h, {}};
        stack.cels[layer].image.pixels.resize(bytes);
        auto *data = stack.cels[layer].image.pixels.data();
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
            {
                const auto i = (y * w + x) * 4;
                data[i] = std::uint8_t(layer ? 220 : 20 + x * 3);
                data[i + 1] = std::uint8_t(layer ? 40 + y * 2 : 170 - y * 4);
                data[i + 2] = std::uint8_t(layer ? 180 - x * 5 : 60 + x * 2);
                data[i + 3] = layer ? ((x + y) % 3 == 0 ? 0 : (x + y) % 4 == 0 ? 255 : 128) : 255;
            }
        std::memcpy(static_cast<std::uint8_t *>(mapped) + layer * bytes, data, bytes);
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(test.device, &sampler, nullptr, &test.sampler));
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = 1;
    set.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(test.device, &set, nullptr, &test.setLayout));
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &test.setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(test.device, &layout, nullptr, &test.layout));
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 2;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    check(vkCreateDescriptorPool(test.device, &pool, nullptr, &test.descriptors));
    std::array<VkDescriptorSet, 2> descriptors{};
    for (int i = 0; i < 2; ++i)
    {
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = test.descriptors;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &test.setLayout;
        check(vkAllocateDescriptorSets(test.device, &allocation, &descriptors[i]));
        const VkDescriptorImageInfo target{test.sampler, test.views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptors[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &target;
        vkUpdateDescriptorSets(test.device, 1, &write, 0, nullptr);
    }
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = dependencies;
    check(vkCreateRenderPass(test.device, &pass, nullptr, &test.pass));
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = test.pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &test.view;
    framebuffer.width = w;
    framebuffer.height = h;
    framebuffer.layers = 1;
    check(vkCreateFramebuffer(test.device, &framebuffer, nullptr, &test.framebuffer));
    const auto module = [&](const char *path, VkShaderModule &out) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() % 4)
            throw std::runtime_error("Invalid cel shader.");
        std::vector<std::uint32_t> words(std::size_t(file.tellg()) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(words.data()), words.size() * 4);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = words.size() * 4;
        info.pCode = words.data();
        check(vkCreateShaderModule(test.device, &info, nullptr, &out));
    };
    module(argv[1], test.shader);
    module(argv[2], test.fragment);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &stage : stages)
    {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = test.shader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = test.fragment;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo sample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = true;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &sample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = test.layout;
    pipeline.renderPass = test.pass;
    check(vkCreateGraphicsPipelines(test.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &test.pipeline));
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commands.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commands.queueFamilyIndex = family;
    check(vkCreateCommandPool(test.device, &commands, nullptr, &test.commands));
    VkCommandBuffer command{};
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = test.commands;
    commandInfo.commandBufferCount = 1;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    check(vkAllocateCommandBuffers(test.device, &commandInfo, &command));
    const auto begin = [&] {
        check(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &info));
    };
    const auto submit = [&] {
        check(vkEndCommandBuffer(command));
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
        check(vkQueueWaitIdle(queue));
    };
    begin();
    for (int i = 0; i < 2; ++i)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = test.textures[i];
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.bufferOffset = i * bytes;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(command, test.buffer, test.textures[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }
    submit();
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        begin();
        VkClearValue clear{};
        clear.color = {{37.f / 255, 41.f / 255, 45.f / 255, 1}};
        VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        passBegin.renderPass = test.pass;
        passBegin.framebuffer = test.framebuffer;
        passBegin.renderArea = {{0, 0}, {w, h}};
        passBegin.clearValueCount = 1;
        passBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, test.pipeline);
        VkViewport viewport{0, 0, float(w), float(h), 0, 1};
        VkRect2D scissor = scenario == 3 ? VkRect2D{{2, 3}, {11, 7}} : VkRect2D{{0, 0}, {w, h}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        struct Push
        {
            float rect[4], surface[2], image[2];
            std::int32_t checker;
        } push{{0, 0, float(w), float(h)}, {float(w), float(h)}, {float(w), float(h)}, 1};
        if (scenario == 3)
        {
            push.rect[0] = 1;
            push.rect[1] = 2;
            push.rect[2] = 13;
            push.rect[3] = 9;
        }
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, test.layout, 0, 1, &descriptors[0], 0,
                                nullptr);
        vkCmdPushConstants(command, test.layout, pushRange.stageFlags, 0, sizeof(push), &push);
        vkCmdDraw(command, 6, 1, 0, 0);
        push.checker = 0;
        vkCmdPushConstants(command, test.layout, pushRange.stageFlags, 0, sizeof(push), &push);
        std::vector<int> order = scenario == 1   ? std::vector<int>{1}
                                 : scenario == 2 ? std::vector<int>{1, 0}
                                                 : std::vector<int>{0, 1};
        for (auto i : order)
        {
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, test.layout, 0, 1, &descriptors[i], 0,
                                    nullptr);
            vkCmdDraw(command, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(command);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(command, test.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, test.buffer, 1, &copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                             nullptr, 0, nullptr);
        submit();
        CelStack expected;
        expected.cels[0].image = {w, h, std::vector<std::uint8_t>(bytes)};
        auto *background = expected.cels[0].image.pixels.data();
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
            {
                auto *pixel = background + (y * w + x) * 4;
                pixel[0] = pixel[1] = pixel[2] = ((x / 8 + y / 8) & 1) ? 69 : 51;
                pixel[3] = 255;
            }
        for (auto i : order)
            expected.cels.push_back(stack.cels[i]);
        const auto *actual = static_cast<const std::uint8_t *>(mapped);
        for (int y = 0; y < int(h); ++y)
            for (int x = 0; x < int(w); ++x)
            {
                std::array<std::uint8_t, 4> wanted{45, 41, 37, 255};
                if (x >= scissor.offset.x && y >= scissor.offset.y &&
                    x < int(scissor.offset.x + scissor.extent.width) &&
                    y < int(scissor.offset.y + scissor.extent.height))
                {
                    const auto sx = std::uint32_t(std::floor((x + 0.5 - push.rect[0]) * w / push.rect[2]));
                    const auto sy = std::uint32_t(std::floor((y + 0.5 - push.rect[1]) * h / push.rect[3]));
                    wanted = sampleCels(expected, sx, sy);
                }
                for (int c = 0; c < 4; ++c)
                    if (std::abs(int(actual[(y * w + x) * 4 + c]) - int(wanted[c])) > 1)
                        throw std::runtime_error("Cel graphics differs from reference in scenario " +
                                                 std::to_string(scenario));
            }
    }
    vkUnmapMemory(test.device, test.bufferMemory);
    std::cout << "GPU cel alpha/order/visibility, checkerboard, nearest sampling and scissor clipping passed on "
              << properties.deviceName << ".\n";
}
