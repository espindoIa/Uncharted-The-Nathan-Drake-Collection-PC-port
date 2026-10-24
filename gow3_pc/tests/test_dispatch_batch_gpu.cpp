// SPDX-License-Identifier: GPL-2.0-or-later
// Small headless GPU correctness check: 16 instances, independent input/output/push bindings,
// scalar/vector builtin Y reads, NumWorkgroups and OpArrayLength, exact CPU reference.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static void Check(VkResult result) { assert(result == VK_SUCCESS); }
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    assert(file.good());
    std::vector<uint32_t> code(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    Check(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    assert(count);
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice gpu = devices[0];
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID == 0x10de) { gpu = candidate; break; }
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu, &properties);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(gpu, &features);
    assert(features.shaderStorageBufferArrayDynamicIndexing);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());
    uint32_t family = 0;
    while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    assert(family < count);
    const float priority = 1;
    const VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const VkPhysicalDeviceFeatures enabled{.shaderStorageBufferArrayDynamicIndexing = VK_TRUE};
    const VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &enabled};
    VkDevice device;
    Check(vkCreateDevice(gpu, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    constexpr uint32_t N = 16;
    const uint64_t alignment = std::max<uint64_t>(256, properties.limits.minStorageBufferOffsetAlignment);
    const uint64_t bytes = 3 * N * alignment;
    const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    Check(vkCreateBuffer(device, &bci, nullptr, &buffer));
    VkMemoryRequirements required;
    vkGetBufferMemoryRequirements(device, buffer, &required);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory_properties);
    uint32_t type = 0;
    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < memory_properties.memoryTypeCount &&
        (!(required.memoryTypeBits & (1u << type)) ||
         (memory_properties.memoryTypes[type].propertyFlags & wanted) != wanted)) ++type;
    assert(type < memory_properties.memoryTypeCount);
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size, .memoryTypeIndex = type};
    VkDeviceMemory memory;
    Check(vkAllocateMemory(device, &mai, nullptr, &memory));
    Check(vkBindBufferMemory(device, buffer, memory, 0));
    void* mapped;
    Check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    std::memset(mapped, 0, bytes);
    auto data = [&](uint32_t area, uint32_t i) {
        return reinterpret_cast<uint32_t*>(static_cast<char*>(mapped) + (area * N + i) * alignment);
    };
    for (uint32_t i = 0; i < N; ++i)
        for (uint32_t x = 0; x < 4; ++x) data(0, i)[x] = i * 1000 + x + 17;
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, N, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo dlci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout descriptors;
    Check(vkCreateDescriptorSetLayout(device, &dlci, nullptr, &descriptors));
    const VkPipelineLayoutCreateInfo plci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &descriptors};
    VkPipelineLayout layout;
    Check(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
    const VkShaderModuleCreateInfo smci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * 4, .pCode = code.data()};
    VkShaderModule module;
    Check(vkCreateShaderModule(device, &smci, nullptr, &module));
    const VkComputePipelineCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
    VkPipeline pipeline;
    Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * N};
    const VkDescriptorPoolCreateInfo dpci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool;
    Check(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
    const VkDescriptorSetAllocateInfo dsai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &descriptors};
    VkDescriptorSet set;
    Check(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo infos[3][N];
    VkWriteDescriptorSet writes[3];
    for (uint32_t area = 0; area < 3; ++area) {
        for (uint32_t i = 0; i < N; ++i) infos[area][i] = {buffer, (area * N + i) * alignment, area == 2 ? 4u : 16u};
        writes[area] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = area,
            .descriptorCount = N, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = infos[area]};
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    const VkCommandPoolCreateInfo cpci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool commands;
    Check(vkCreateCommandPool(device, &cpci, nullptr, &commands));
    const VkCommandBufferAllocateInfo cbai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    Check(vkAllocateCommandBuffers(device, &cbai, &cmd));
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, 1, N, 1);
    const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
        1, &barrier, 0, nullptr, 0, nullptr);
    Check(vkEndCommandBuffer(cmd));
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    Check(vkQueueWaitIdle(queue));
    for (uint32_t i = 0; i < N; ++i)
        for (uint32_t x = 0; x < 4; ++x) assert(data(1, i)[x] == i * 1000 + x + 17);
    std::printf("dispatch batch GPU: 16 instances x 4 values EXACT on %s\n", properties.deviceName);
    vkDestroyCommandPool(device, commands, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}
