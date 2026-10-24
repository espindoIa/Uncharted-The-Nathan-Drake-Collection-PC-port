// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_dispatch_batch_spirv.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

ComputePipeline::~ComputePipeline() = default;

bool ComputePipeline::EnsureBatchPipeline() const {
    if (batch_attempted) return bool(batch_pipeline);
    batch_attempted = true;
    const auto& info = GetStage(Shader::SwStage::Compute);
    const u32 bindings = info.buffers.size() + 1; // extra array replaces PushData
    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    if (info.pgm_hash != 0x8f067540 || info.buffers.size() != 10 ||
        !info.images.empty() || !info.samplers.empty() ||
        !instance.SupportsStorageBufferArrayIndexing() ||
        bindings * BatchCapacity > limits.maxPerStageDescriptorStorageBuffers ||
        bindings * BatchCapacity > limits.maxDescriptorSetStorageBuffers ||
        bindings * BatchCapacity > limits.maxPerStageResources) return false;
    for (u32 i = 0; i < 9; ++i) {
        if (info.buffers[i].IsSpecial() || bool(info.buffers[i].is_written) != (i >= 6))
            return false;
    }
    if (info.buffers[9].buffer_type != Shader::BufferType::Flatbuf) return false;
    const auto binary = LoadBatchComputeBinary(info.pgm_hash, compute_key.value);
    std::string problem;
    const auto batch = MakeDispatchBatchSpirv(binary, BatchCapacity, info.buffers.size(), problem);
    if (batch.empty()) {
        std::printf("Dispatch fusion: shader %llx not eligible: %s\n", info.pgm_hash, problem.c_str());
        return false;
    }
    const auto device = instance.GetDevice();
    const vk::ShaderModule module = CompileSPV(batch, device);
    std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
    for (u32 binding = 0; binding < bindings; ++binding) {
        layout_bindings.push_back({.binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = BatchCapacity, .stageFlags = vk::ShaderStageFlagBits::eCompute});
    }
    auto [desc_result, descriptors] = device.createDescriptorSetLayoutUnique(
        {.bindingCount = bindings, .pBindings = layout_bindings.data()});
    if (desc_result != vk::Result::eSuccess) { device.destroyShaderModule(module); return false; }
    batch_descriptors = std::move(descriptors);
    const vk::DescriptorSetLayout descriptor_layout = *batch_descriptors;
    auto [layout_result, layout] = device.createPipelineLayoutUnique(
        {.setLayoutCount = 1, .pSetLayouts = &descriptor_layout});
    if (layout_result != vk::Result::eSuccess) { device.destroyShaderModule(module); return false; }
    batch_layout = std::move(layout);
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize = 64};
    const vk::PipelineShaderStageCreateInfo shader{
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"};
    auto [result, pipeline] = device.createComputePipelineUnique({}, {.stage = shader, .layout = *batch_layout});
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) return false;
    batch_pipeline = std::move(pipeline);
    std::printf("Dispatch fusion: shader %llx validated, %u instances per dispatch, %u bindings\n",
                info.pgm_hash, BatchCapacity, bindings);
    return true;
}

vk::DescriptorSet ComputePipeline::BatchDescriptorSet() const {
    return desc_heap.Commit(*batch_descriptors);
}

} // namespace Vulkan
